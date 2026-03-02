using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MftScanner
{
    public class MftParser
    {
        private const int Magic = unchecked((int)0x55AA55AA);
        private const long MaxFileSize = 200L * 1024 * 1024 * 1024 * 1024;
        private const int MaxNameLength = 32767;

        public static List<FileNode> Parse(string binPath)
        {
            if (!File.Exists(binPath)) throw new FileNotFoundException("找不到 dump 文件", binPath);

            var nodeLookup = new Dictionary<long, FileNode>(500000);
            var parentLookup = new Dictionary<long, long>(500000);

            using (var fs = new FileStream(binPath, FileMode.Open, FileAccess.Read))
            using (var br = new BinaryReader(fs))
            {
                if (fs.Length < 8) throw new Exception("结果文件过小或损坏。");

                int magic = br.ReadInt32();
                if (magic != Magic)
                {
                    throw new Exception("文件格式错误：magic 不匹配。");
                }

                int count = br.ReadInt32();
                if (count < 0) throw new Exception("文件格式错误：记录数非法。");

                const int minRecordBytes = sizeof(long) + sizeof(long) + sizeof(long) + sizeof(uint) + sizeof(int);
                long maxPossibleCount = (fs.Length - 8) / minRecordBytes;
                if (count > maxPossibleCount) throw new Exception("文件格式错误：记录数超出文件长度。");

                for (int i = 0; i < count; i++)
                {
                    if (fs.Position + minRecordBytes > fs.Length)
                        throw new EndOfStreamException("结果文件在记录头部处提前结束。");

                    long id = br.ReadInt64();
                    long parentId = br.ReadInt64();
                    long size = br.ReadInt64();
                    uint attributes = br.ReadUInt32();

                    int nameLen = br.ReadInt32();
                    if (nameLen < 0 || nameLen > MaxNameLength)
                        throw new Exception($"文件格式错误：非法名称长度 {nameLen}。");

                    string name = string.Empty;
                    if (nameLen > 0)
                    {
                        long nameByteLen = nameLen * 2L;
                        if (fs.Position + nameByteLen > fs.Length)
                            throw new EndOfStreamException("结果文件在文件名字段处提前结束。");

                        byte[] nameBytes = br.ReadBytes(nameLen * 2);
                        name = Encoding.Unicode.GetString(nameBytes);
                    }

                    if (size < 0 || size > MaxFileSize) size = 0;

                    var node = new FileNode
                    {
                        ID = id,
                        Name = name,
                        Size = size,
                        IsDirectory = (attributes & 0x02) != 0,
                        Children = new List<FileNode>()
                    };

                    if (!nodeLookup.ContainsKey(id))
                    {
                        nodeLookup[id] = node;
                        parentLookup[id] = parentId;
                    }
                }
            }

            if (!nodeLookup.ContainsKey(5))
            {
                nodeLookup[5] = new FileNode { ID = 5, Name = ".", IsDirectory = true, Children = new List<FileNode>() };
            }

            var roots = new List<FileNode>();

            foreach (var kvp in nodeLookup)
            {
                long id = kvp.Key;
                FileNode node = kvp.Value;

                if (id == 5) continue;

                if (parentLookup.TryGetValue(id, out long parentId) &&
                    nodeLookup.TryGetValue(parentId, out FileNode? parent) &&
                    parent != null)
                {
                    parent.Children.Add(node);
                    node.Parent = parent;
                }
                else
                {
                    nodeLookup[5].Children.Add(node);
                    node.Parent = nodeLookup[5];
                }
            }

            roots.Add(nodeLookup[5]);

            foreach (var root in roots)
            {
                CalculateDirectorySize(root, new HashSet<long>());
            }

            return roots;
        }

        private static long CalculateDirectorySize(FileNode node, HashSet<long> visiting)
        {
            if (!node.IsDirectory) return node.Size;
            if (!visiting.Add(node.ID)) return 0;

            long total = 0;
            foreach (var child in node.Children)
            {
                total += CalculateDirectorySize(child, visiting);
            }

            visiting.Remove(node.ID);
            node.Size = total;
            return total;
        }
    }
}
