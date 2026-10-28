using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

internal static class Program
{
    private const int PageSize = 128;
    private const int HeaderSize = 128;
    private const int MagicOffset = 0;
    private const int MagicLen = 7;
    private const int PartOffset = 17;
    private const int PartLen = 10;
    private const int CrcOffset = 31;
    private const int SizeOffset = 35;
    private const uint App1StartAddress = 0x08010000U;
    private const uint App1EndAddress = 0x0803F000U;
    private const uint SramStartAddress = 0x20000000U;
    private const uint SramEndAddress = 0x20024000U;
    private const int App1S19DataSize = 32;

    private static int Main(string[] args)
    {
        try
        {
            Options options = Options.Parse(args);
            byte[] raw = File.ReadAllBytes(options.InputPath);
            bool generateApp1S19 = !string.IsNullOrWhiteSpace(options.App1S19Path) &&
                                   !string.IsNullOrWhiteSpace(options.FactoryHexPath);

            if (generateApp1S19)
            {
                if (File.Exists(options.App1S19Path))
                {
                    File.Delete(options.App1S19Path);
                }
                ValidateApp1Image(raw);
            }

            Pack(options, raw);
            GenerateFactoryHexIfNeeded(options);
            GenerateApp1S19IfNeeded(options, raw);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("tool error: " + ex.Message);
            Console.Error.WriteLine(Options.Usage);
            return 1;
        }
    }

    private static void Pack(Options options, byte[] raw)
    {
        byte[] header = BuildHeader(raw, options.PartNumber, options.Magic);
        int payloadSize = AlignUp(raw.Length, PageSize);
        string outDir = Path.GetDirectoryName(Path.GetFullPath(options.OutputPath));
        uint crc32 = Crc32(raw);

        if (!string.IsNullOrEmpty(outDir))
        {
            Directory.CreateDirectory(outDir);
        }

        using (FileStream output = File.Create(options.OutputPath))
        {
            output.Write(header, 0, header.Length);
            output.Write(raw, 0, raw.Length);

            for (int i = raw.Length; i < payloadSize; i++)
            {
                output.WriteByte(0xFF);
            }
        }

        AppendLog(options, crc32, HeaderSize + payloadSize);

        Console.WriteLine(Path.GetFullPath(options.OutputPath));
        Console.WriteLine("input : " + Path.GetFullPath(options.InputPath));
        Console.WriteLine("part  : " + options.PartNumber);
        Console.WriteLine("size  : " + raw.Length + " bytes");
        Console.WriteLine("crc32 : 0x" + crc32.ToString("X8"));
        Console.WriteLine("total : " + (HeaderSize + payloadSize) + " bytes");
    }

    private static void ValidateApp1Image(byte[] app)
    {
        uint stackPointer;
        uint resetVector;
        uint resetAddress;

        if (app.Length < 8)
        {
            throw new ArgumentException("APP BIN太短，缺少向量表");
        }

        if ((uint)app.Length > (App1EndAddress - App1StartAddress))
        {
            throw new ArgumentException("APP BIN超出APP1空间");
        }

        stackPointer = ReadLe32(app, 0);
        resetVector = ReadLe32(app, 4);
        if ((stackPointer < SramStartAddress) || (stackPointer > SramEndAddress))
        {
            throw new ArgumentException("APP初始栈指针不在SRAM范围内: 0x" + stackPointer.ToString("X8"));
        }

        resetAddress = resetVector & ~1U;
        if ((resetAddress < App1StartAddress) ||
            (resetAddress >= (App1StartAddress + (uint)app.Length)))
        {
            throw new ArgumentException("APP复位向量不在APP1镜像内: 0x" + resetVector.ToString("X8"));
        }

        if ((resetVector & 1U) == 0U)
        {
            throw new ArgumentException("APP复位向量不是Thumb地址: 0x" + resetVector.ToString("X8"));
        }
    }

    private static uint ReadLe32(byte[] data, int offset)
    {
        return (uint)data[offset] |
               ((uint)data[offset + 1] << 8) |
               ((uint)data[offset + 2] << 16) |
               ((uint)data[offset + 3] << 24);
    }

    private static void GenerateApp1S19IfNeeded(Options options, byte[] app)
    {
        if (string.IsNullOrWhiteSpace(options.App1S19Path) ||
            string.IsNullOrWhiteSpace(options.FactoryHexPath))
        {
            return;
        }

        string outputDirectory = Path.GetDirectoryName(Path.GetFullPath(options.App1S19Path));
        if (!string.IsNullOrEmpty(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
        }

        using (StreamWriter writer = new StreamWriter(options.App1S19Path, false, Encoding.ASCII))
        {
            writer.WriteLine(CreateSRecord(0, 0U, 2, Encoding.ASCII.GetBytes("DUCAR-APP1")));

            uint recordCount = 0U;
            for (int offset = 0; offset < app.Length; offset += App1S19DataSize)
            {
                int count = Math.Min(App1S19DataSize, app.Length - offset);
                byte[] data = new byte[count];
                Array.Copy(app, offset, data, 0, count);
                writer.WriteLine(CreateSRecord(3, App1StartAddress + (uint)offset, 4, data));
                recordCount++;
            }

            writer.WriteLine(CreateSRecord(5, recordCount, 2, new byte[0]));
            writer.WriteLine(CreateSRecord(7, ReadLe32(app, 4), 4, new byte[0]));
        }

        Console.WriteLine("APP1 S19     : " + Path.GetFullPath(options.App1S19Path));
    }

    private static string CreateSRecord(int recordType, uint address, int addressSize, byte[] data)
    {
        List<byte> fields = new List<byte>(addressSize + data.Length + 2);
        int sum;

        fields.Add((byte)(addressSize + data.Length + 1));
        for (int shift = (addressSize - 1) * 8; shift >= 0; shift -= 8)
        {
            fields.Add((byte)((address >> shift) & 0xFFU));
        }
        fields.AddRange(data);

        sum = 0;
        for (int i = 0; i < fields.Count; i++)
        {
            sum += fields[i];
        }
        fields.Add((byte)(~sum & 0xFF));

        StringBuilder record = new StringBuilder(2 + (fields.Count * 2));
        record.Append('S');
        record.Append(recordType.ToString());
        for (int i = 0; i < fields.Count; i++)
        {
            record.Append(fields[i].ToString("X2"));
        }
        return record.ToString();
    }

    private static void GenerateFactoryHexIfNeeded(Options options)
    {
        string bootHexForMerge;
        HexImage image;

        if (string.IsNullOrWhiteSpace(options.FactoryHexPath))
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(options.BootHexPath) || string.IsNullOrWhiteSpace(options.AppHexPath))
        {
            throw new ArgumentException("factory_hex需要同时配置boot_hex和app_hex");
        }

        if (!File.Exists(options.BootHexPath))
        {
            throw new FileNotFoundException("找不到BOOT HEX", options.BootHexPath);
        }

        if (!File.Exists(options.AppHexPath))
        {
            throw new FileNotFoundException("找不到APP HEX", options.AppHexPath);
        }

        bootHexForMerge = options.CopyBootHexPath;
        if (string.IsNullOrWhiteSpace(bootHexForMerge))
        {
            bootHexForMerge = Path.Combine(Options.ToolDir, Path.GetFileName(options.BootHexPath));
        }

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(bootHexForMerge)));
        File.Copy(options.BootHexPath, bootHexForMerge, true);

        image = new HexImage();
        image.Merge(HexImage.Read(bootHexForMerge), "BOOT");
        image.Merge(HexImage.Read(options.AppHexPath), "APP1");
        image.Write(options.FactoryHexPath);

        Console.WriteLine("boot hex copied: " + Path.GetFullPath(bootHexForMerge));
        Console.WriteLine("factory hex    : " + Path.GetFullPath(options.FactoryHexPath));
    }

    private static byte[] BuildHeader(byte[] raw, string partNumber, string magic)
    {
        byte[] header = new byte[HeaderSize];
        byte[] magicBytes = EncodeAscii(magic, "magic");
        byte[] partBytes = EncodeAscii(partNumber, "part");

        for (int i = 0; i < header.Length; i++)
        {
            header[i] = 0xFF;
        }

        if (magicBytes.Length > MagicLen)
        {
            throw new ArgumentException("magic长度不能超过" + MagicLen + "字节");
        }

        if (partBytes.Length > PartLen)
        {
            throw new ArgumentException("品号长度不能超过" + PartLen + "字节");
        }

        Array.Copy(magicBytes, 0, header, MagicOffset, magicBytes.Length);
        Array.Copy(partBytes, 0, header, PartOffset, partBytes.Length);
        WriteLe32(header, CrcOffset, Crc32(raw));
        WriteLe32(header, SizeOffset, checked((uint)raw.Length));
        return header;
    }

    private static byte[] EncodeAscii(string text, string name)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            throw new ArgumentException(name + "不能为空");
        }

        for (int i = 0; i < text.Length; i++)
        {
            if (text[i] > 0x7F)
            {
                throw new ArgumentException(name + "只能使用ASCII字符");
            }
        }

        return Encoding.ASCII.GetBytes(text);
    }

    private static void WriteLe32(byte[] data, int offset, uint value)
    {
        data[offset + 0] = (byte)(value & 0xFFU);
        data[offset + 1] = (byte)((value >> 8) & 0xFFU);
        data[offset + 2] = (byte)((value >> 16) & 0xFFU);
        data[offset + 3] = (byte)((value >> 24) & 0xFFU);
    }

    private static int AlignUp(int value, int align)
    {
        return ((value + align - 1) / align) * align;
    }

    private static uint Crc32(byte[] data)
    {
        uint crc = 0xFFFFFFFFU;

        for (int i = 0; i < data.Length; i++)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; bit++)
            {
                crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
            }
        }

        return ~crc;
    }

    private static void AppendLog(Options options, uint crc32, int totalSize)
    {
        string logPath = Path.Combine(Options.ToolDir, "log.txt");
        string outputPath = Path.GetFullPath(options.OutputPath);
        StringBuilder log = new StringBuilder();

        log.AppendLine(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
        log.AppendLine(Path.GetFileName(outputPath) + ": " + Md5File(outputPath));
        log.AppendLine("app_bin: " + Path.GetFullPath(options.InputPath));
        log.AppendLine("profile: " + (string.IsNullOrEmpty(options.Profile) ? "-" : options.Profile));
        log.AppendLine("part: " + options.PartNumber);
        log.AppendLine("crc32: 0x" + crc32.ToString("X8"));
        log.AppendLine("total: " + totalSize + " bytes");
        log.AppendLine();

        File.AppendAllText(logPath, log.ToString(), new UTF8Encoding(false));
    }

    private static string Md5File(string path)
    {
        using (MD5 md5 = MD5.Create())
        using (FileStream stream = File.OpenRead(path))
        {
            byte[] hash = md5.ComputeHash(stream);
            StringBuilder text = new StringBuilder(hash.Length * 2);

            for (int i = 0; i < hash.Length; i++)
            {
                text.Append(hash[i].ToString("x2"));
            }

            return text.ToString();
        }
    }

    private sealed class HexImage
    {
        private readonly SortedDictionary<uint, byte> bytes = new SortedDictionary<uint, byte>();

        public static HexImage Read(string path)
        {
            HexImage image = new HexImage();
            uint upper = 0U;
            int lineNo = 0;

            foreach (string rawLine in File.ReadAllLines(path, Encoding.ASCII))
            {
                string line = rawLine.Trim();
                byte[] record;
                int len;
                uint addr;
                int type;

                lineNo++;
                if (line.Length == 0)
                {
                    continue;
                }

                if ((line[0] != ':') || (((line.Length - 1) % 2) != 0))
                {
                    throw new ArgumentException(Path.GetFileName(path) + "第" + lineNo + "行不是Intel HEX记录");
                }

                record = HexToBytes(line.Substring(1));
                if (record.Length < 5)
                {
                    throw new ArgumentException(Path.GetFileName(path) + "第" + lineNo + "行长度错误");
                }

                if ((Sum(record) & 0xFF) != 0)
                {
                    throw new ArgumentException(Path.GetFileName(path) + "第" + lineNo + "行校验和错误");
                }

                len = record[0];
                addr = (uint)((record[1] << 8) | record[2]);
                type = record[3];
                if (record.Length != (5 + len))
                {
                    throw new ArgumentException(Path.GetFileName(path) + "第" + lineNo + "行数据长度不匹配");
                }

                if (type == 0x00)
                {
                    for (int i = 0; i < len; i++)
                    {
                        image.Put((upper << 16) + addr + (uint)i, record[4 + i]);
                    }
                }
                else if (type == 0x01)
                {
                    break;
                }
                else if (type == 0x04)
                {
                    if (len != 2)
                    {
                        throw new ArgumentException(Path.GetFileName(path) + "第" + lineNo + "行扩展地址长度错误");
                    }

                    upper = (uint)((record[4] << 8) | record[5]);
                }
            }

            return image;
        }

        public void Merge(HexImage source, string name)
        {
            foreach (KeyValuePair<uint, byte> item in source.bytes)
            {
                Put(item.Key, item.Value);
            }
        }

        public void WriteLe32(uint addr, uint value)
        {
            Put(addr + 0U, (byte)(value & 0xFFU));
            Put(addr + 1U, (byte)((value >> 8) & 0xFFU));
            Put(addr + 2U, (byte)((value >> 16) & 0xFFU));
            Put(addr + 3U, (byte)((value >> 24) & 0xFFU));
        }

        public void Write(string path)
        {
            string outDir = Path.GetDirectoryName(Path.GetFullPath(path));

            if (!string.IsNullOrEmpty(outDir))
            {
                Directory.CreateDirectory(outDir);
            }

            using (StreamWriter writer = new StreamWriter(path, false, Encoding.ASCII))
            {
                uint currentUpper = 0xFFFFFFFFU;
                uint previousAddr = 0xFFFFFFFFU;
                uint chunkAddr = 0U;
                List<byte> chunk = new List<byte>(16);

                foreach (KeyValuePair<uint, byte> item in bytes)
                {
                    uint upper = item.Key >> 16;
                    uint low = item.Key & 0xFFFFU;
                    bool needFlush = (chunk.Count > 0) &&
                                     ((upper != currentUpper) ||
                                      (item.Key != (previousAddr + 1U)) ||
                                      (chunk.Count >= 16));

                    if (needFlush)
                    {
                        WriteDataRecord(writer, chunkAddr, chunk);
                        chunk.Clear();
                    }

                    if (upper != currentUpper)
                    {
                        WriteExtLinearRecord(writer, upper);
                        currentUpper = upper;
                    }

                    if (chunk.Count == 0)
                    {
                        chunkAddr = low;
                    }

                    chunk.Add(item.Value);
                    previousAddr = item.Key;
                }

                if (chunk.Count > 0)
                {
                    WriteDataRecord(writer, chunkAddr, chunk);
                }

                writer.WriteLine(":00000001FF");
            }
        }

        private void Put(uint addr, byte value)
        {
            byte oldValue;

            if (bytes.TryGetValue(addr, out oldValue) && (oldValue != value))
            {
                throw new ArgumentException("HEX地址重复且数据不同: 0x" + addr.ToString("X8"));
            }

            bytes[addr] = value;
        }

        private static byte[] HexToBytes(string text)
        {
            byte[] data = new byte[text.Length / 2];

            for (int i = 0; i < data.Length; i++)
            {
                data[i] = Convert.ToByte(text.Substring(i * 2, 2), 16);
            }

            return data;
        }

        private static int Sum(byte[] data)
        {
            int sum = 0;

            for (int i = 0; i < data.Length; i++)
            {
                sum += data[i];
            }

            return sum;
        }

        private static void WriteExtLinearRecord(StreamWriter writer, uint upper)
        {
            WriteRecord(writer, 0U, 0x04, new byte[] { (byte)((upper >> 8) & 0xFFU), (byte)(upper & 0xFFU) });
        }

        private static void WriteDataRecord(StreamWriter writer, uint addr, List<byte> data)
        {
            WriteRecord(writer, addr, 0x00, data.ToArray());
        }

        private static void WriteRecord(StreamWriter writer, uint addr, int type, byte[] data)
        {
            int sum = data.Length + (int)((addr >> 8) & 0xFFU) + (int)(addr & 0xFFU) + type;
            StringBuilder line = new StringBuilder();

            line.Append(':');
            line.Append(data.Length.ToString("X2"));
            line.Append(((int)addr & 0xFFFF).ToString("X4"));
            line.Append(type.ToString("X2"));
            for (int i = 0; i < data.Length; i++)
            {
                sum += data[i];
                line.Append(data[i].ToString("X2"));
            }

            line.Append(((~sum + 1) & 0xFF).ToString("X2"));
            writer.WriteLine(line.ToString());
        }
    }

    private sealed class Options
    {
        public static readonly string ToolDir = Path.GetFullPath(AppDomain.CurrentDomain.BaseDirectory);
        private static readonly string ProjectRoot = new DirectoryInfo(ToolDir).Parent.FullName;

        public string Profile = "";
        public string InputPath = "";
        public string OutputPath = "";
        public string OutputBase = "";
        public string OutputDir = "";
        public string PartNumber = "";
        public string SoftwarePn = "";
        public string BatteryType = "";
        public string SoftwareVersionSub = "";
        public string SoftwareVersion = "";
        public string Magic = "DUCAR";
        public string AppParamPath = "";
        public string BootHexPath = "";
        public string AppHexPath = "";
        public string CopyBootHexPath = "";
        public string FactoryHexPath = "";
        public string App1S19Path = "";
        private bool inputPathFromArgs;
        private bool factoryHexFromArgs;

        public const string Usage =
            "用法1: tool.exe -p n32\n" +
            "用法2: tool.exe --input <raw_app.bin> --output <iap_package.bin> --part <10字节品号> [--magic DUCAR]\n" +
            "兼容GD: tool.exe -p n32 -bin <app.bin> -out <输出基础名> -s2 <品号> -n <型号> -t <T版本> -v <V版本>";

        public static Options Parse(string[] args)
        {
            Options options = new Options();

            options.Profile = FindProfile(args);
            if (!string.IsNullOrWhiteSpace(options.Profile))
            {
                options.ApplyProfile(options.Profile);
            }

            options.ApplyArgs(args);
            options.Normalize();
            return options;
        }

        private static string FindProfile(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                if ((args[i] == "-p") || (args[i] == "--profile"))
                {
                    if (i + 1 >= args.Length)
                    {
                        throw new ArgumentException(args[i] + "缺少参数");
                    }

                    return args[i + 1];
                }
            }

            return "";
        }

        private void ApplyProfile(string profile)
        {
            string configPath = Path.Combine(ToolDir, "config.json");
            JavaScriptSerializer serializer = new JavaScriptSerializer();
            Dictionary<string, object> root;
            Dictionary<string, object> node;

            if (!File.Exists(configPath))
            {
                throw new FileNotFoundException("找不到tool配置文件", configPath);
            }

            root = serializer.Deserialize<Dictionary<string, object>>(File.ReadAllText(configPath, Encoding.UTF8));
            if ((root == null) || (!root.ContainsKey(profile)))
            {
                throw new ArgumentException("config.json中找不到profile: " + profile);
            }

            node = AsDictionary(root[profile], profile);
            FillString(node, new string[] { "software_pn", "ssn", "part_number", "part" }, delegate(string value) { SoftwarePn = value; });
            FillString(node, new string[] { "bat_type", "battery_type", "name" }, delegate(string value) { BatteryType = value; });
            FillString(node, new string[] { "software_version_sub", "version_sub", "t" }, delegate(string value) { SoftwareVersionSub = value; });
            FillString(node, new string[] { "software_version", "version", "v" }, delegate(string value) { SoftwareVersion = value; });
            FillString(node, new string[] { "magic" }, delegate(string value) { Magic = value; });
            FillString(node, new string[] { "app_param_h", "app_param", "param_h" }, delegate(string value) { AppParamPath = ResolveProjectPath(value); });
            FillString(node, new string[] { "app_bin", "input_bin", "bin" }, delegate(string value) { InputPath = ResolveProjectPath(value); });
            FillString(node, new string[] { "boot_hex" }, delegate(string value) { BootHexPath = ResolveProjectPath(value); });
            FillString(node, new string[] { "app_hex" }, delegate(string value) { AppHexPath = ResolveProjectPath(value); });
            FillString(node, new string[] { "copy_boot_hex", "tool_boot_hex" }, delegate(string value) { CopyBootHexPath = ResolveProjectPath(value); });
            FillString(node, new string[] { "factory_hex" }, delegate(string value) { FactoryHexPath = ResolveProjectPath(value); });
            FillString(node, new string[] { "app1_s19" }, delegate(string value) { App1S19Path = ResolveProjectPath(value); });
            FillString(node, new string[] { "output_dir" }, delegate(string value) { OutputDir = ResolveProjectPath(value); });
            FillString(node, new string[] { "output_base" }, delegate(string value) { OutputBase = ResolveProjectPath(value); });
            FillString(node, new string[] { "output" }, delegate(string value) { OutputPath = ResolveProjectPath(value); });

            if (string.IsNullOrWhiteSpace(AppParamPath))
            {
                AppParamPath = ResolveProjectPath("app\\User\\App\\app_param.h");
            }

            ApplyAppParamHeader();
        }

        private void ApplyArgs(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                string arg = args[i];
                string value;

                switch (arg)
                {
                    case "-p":
                    case "--profile":
                        Profile = NextValue(args, ref i, arg);
                        break;

                    case "--input":
                    case "-i":
                    case "-bin":
                        value = NextValue(args, ref i, arg);
                        InputPath = Path.GetFullPath(value);
                        inputPathFromArgs = true;
                        break;

                    case "--output":
                    case "-o":
                        value = NextValue(args, ref i, arg);
                        OutputPath = Path.GetFullPath(value);
                        break;

                    case "-out":
                    case "--output-base":
                        value = NextValue(args, ref i, arg);
                        OutputBase = Path.GetFullPath(value);
                        break;

                    case "--output-dir":
                        value = NextValue(args, ref i, arg);
                        OutputDir = Path.GetFullPath(value);
                        break;

                    case "--part":
                    case "-s":
                        value = NextValue(args, ref i, arg);
                        PartNumber = value;
                        break;

                    case "-s2":
                    case "--software-pn":
                        value = NextValue(args, ref i, arg);
                        SoftwarePn = value;
                        break;

                    case "-n":
                    case "--name":
                        value = NextValue(args, ref i, arg);
                        BatteryType = value;
                        break;

                    case "-t":
                    case "--version-sub":
                        value = NextValue(args, ref i, arg);
                        SoftwareVersionSub = value;
                        break;

                    case "-v":
                    case "--version":
                        value = NextValue(args, ref i, arg);
                        SoftwareVersion = value;
                        break;

                    case "--magic":
                        value = NextValue(args, ref i, arg);
                        Magic = value;
                        break;

                    case "--app-param":
                        value = NextValue(args, ref i, arg);
                        AppParamPath = Path.GetFullPath(value);
                        ApplyAppParamHeader();
                        break;

                    case "--boot-hex":
                        value = NextValue(args, ref i, arg);
                        BootHexPath = Path.GetFullPath(value);
                        break;

                    case "--app-hex":
                        value = NextValue(args, ref i, arg);
                        AppHexPath = Path.GetFullPath(value);
                        break;

                    case "--copy-boot-hex":
                        value = NextValue(args, ref i, arg);
                        CopyBootHexPath = Path.GetFullPath(value);
                        break;

                    case "--factory-hex":
                        value = NextValue(args, ref i, arg);
                        FactoryHexPath = Path.GetFullPath(value);
                        factoryHexFromArgs = true;
                        break;

                    case "-hex":
                    case "-boot":
                    case "-mark":
                        /* 兼容GD工具的参数入口；当前N32升级包只需要生成串口bin。 */
                        NextValue(args, ref i, arg);
                        break;

                    case "--help":
                    case "-h":
                        Console.WriteLine(Usage);
                        Environment.Exit(0);
                        break;

                    default:
                        throw new ArgumentException("未知参数: " + arg);
                }
            }
        }

        private void Normalize()
        {
            if (inputPathFromArgs && !factoryHexFromArgs)
            {
                FactoryHexPath = "";
            }

            if (string.IsNullOrWhiteSpace(PartNumber))
            {
                PartNumber = SoftwarePn;
            }

            if (string.IsNullOrWhiteSpace(InputPath))
            {
                throw new ArgumentException("缺少输入bin，请配置app_bin或使用-bin/--input指定");
            }

            if (!File.Exists(InputPath))
            {
                throw new FileNotFoundException("找不到输入bin", InputPath);
            }

            if (string.IsNullOrWhiteSpace(PartNumber))
            {
                throw new ArgumentException("缺少品号，请配置software_pn或使用-s2/--part指定");
            }

            if (string.IsNullOrWhiteSpace(BatteryType))
            {
                BatteryType = Path.GetFileNameWithoutExtension(InputPath);
            }

            if (string.IsNullOrWhiteSpace(SoftwareVersionSub))
            {
                SoftwareVersionSub = "T0";
            }

            if (string.IsNullOrWhiteSpace(SoftwareVersion))
            {
                SoftwareVersion = "V001";
            }

            SoftwareVersionSub = NormalizeVersionPrefix(SoftwareVersionSub, 'T');
            SoftwareVersion = NormalizeVersionPrefix(SoftwareVersion, 'V');

            if (string.IsNullOrWhiteSpace(OutputPath))
            {
                if (!string.IsNullOrWhiteSpace(OutputBase))
                {
                    OutputPath = OutputBase + ".bin";
                }
                else
                {
                    string dir = string.IsNullOrWhiteSpace(OutputDir) ? ToolDir : OutputDir;
                    string fileName = SanitizeFileName(SoftwarePn + "_" + BatteryType + "_" + SoftwareVersion + "_" + SoftwareVersionSub + ".bin");
                    OutputPath = Path.Combine(dir, fileName);
                }
            }

            if (string.IsNullOrWhiteSpace(FactoryHexPath) &&
                !inputPathFromArgs &&
                !string.IsNullOrWhiteSpace(BootHexPath) &&
                !string.IsNullOrWhiteSpace(AppHexPath))
            {
                FactoryHexPath = Path.ChangeExtension(OutputPath, ".hex");
            }
        }

        private void ApplyAppParamHeader()
        {
            string text;

            if (string.IsNullOrWhiteSpace(AppParamPath) || !File.Exists(AppParamPath))
            {
                return;
            }

            text = File.ReadAllText(AppParamPath, Encoding.UTF8);
            FillAppParamString(text, "APP_PARAM_SOFTWARE_PN", delegate(string value) { SoftwarePn = value; });
            FillAppParamString(text, "APP_PARAM_BAT_TYPE", delegate(string value) { BatteryType = value; });
            FillAppParamString(text, "APP_PARAM_PACKAGE_T_VERSION", delegate(string value) { SoftwareVersionSub = value; });
            FillAppParamString(text, "APP_PARAM_PACKAGE_V_VERSION", delegate(string value) { SoftwareVersion = value; });
        }

        private static void FillAppParamString(string text, string macro, Action<string> setter)
        {
            Match match = Regex.Match(text,
                                      "^\\s*#\\s*define\\s+" + Regex.Escape(macro) + "\\s+\"([^\"]*)\"",
                                      RegexOptions.Multiline);

            if (match.Success)
            {
                setter(match.Groups[1].Value);
            }
        }

        private static string NormalizeVersionPrefix(string value, char prefix)
        {
            string text = value.Trim();

            if ((text.Length > 0) && (char.ToUpperInvariant(text[0]) == prefix))
            {
                return prefix + text.Substring(1);
            }

            return prefix + text;
        }

        private static Dictionary<string, object> AsDictionary(object value, string name)
        {
            Dictionary<string, object> dictionary = value as Dictionary<string, object>;

            if (dictionary == null)
            {
                throw new ArgumentException("config.json中" + name + "不是对象");
            }

            return dictionary;
        }

        private static void FillString(Dictionary<string, object> node, string[] keys, Action<string> setter)
        {
            for (int i = 0; i < keys.Length; i++)
            {
                object value;

                if (node.TryGetValue(keys[i], out value) && (value != null))
                {
                    setter(Convert.ToString(value));
                    return;
                }
            }
        }

        private static string ResolveProjectPath(string value)
        {
            if (string.IsNullOrWhiteSpace(value))
            {
                return value;
            }

            if (Path.IsPathRooted(value))
            {
                return Path.GetFullPath(value);
            }

            return Path.GetFullPath(Path.Combine(ProjectRoot, value));
        }

        private static string SanitizeFileName(string fileName)
        {
            char[] invalid = Path.GetInvalidFileNameChars();
            StringBuilder clean = new StringBuilder(fileName.Length);

            for (int i = 0; i < fileName.Length; i++)
            {
                clean.Append(Array.IndexOf(invalid, fileName[i]) >= 0 ? '_' : fileName[i]);
            }

            return clean.ToString();
        }

        private static string NextValue(string[] args, ref int index, string arg)
        {
            if (index + 1 >= args.Length)
            {
                throw new ArgumentException(arg + "缺少参数");
            }

            index++;
            return args[index];
        }
    }
}
