using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace MftScanner
{
    // C++ 对接结构体
    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Unicode)]
    public struct RawFileInfo
    {
        public long ID;
        public long ParentID;
        public long Size;
        public uint Attributes;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Name;
    }

    // 树节点类
    public class FileNode
    {
        public long ID { get; set; }

        public string? Name { get; set; }
        public long Size { get; set; }
        public bool IsDirectory { get; set; }
        public List<FileNode> Children { get; set; } = new();

        // 【新增】父节点引用，用于回溯生成完整路径
        // System.Text.Json.Serialization.JsonIgnore // 如果有序列化需求需忽略防止循环引用
        public FileNode? Parent { get; set; }
    }
}