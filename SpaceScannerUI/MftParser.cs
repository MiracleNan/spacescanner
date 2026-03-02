using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MftScanner
{
    public class MftParser
    {
        public static List<FileNode> Parse(string binPath)
        {
            if (!File.Exists(binPath)) throw new FileNotFoundException("找不到 dump 文件", binPath);

            var nodeLookup = new Dictionary<long, FileNode>(500000);
            var parentLookup = new Dictionary<long, long>(500000);

            // 使用 FileStream 打开
            using (var fs = new FileStream(binPath, FileMode.Open, FileAccess.Read))
            using (var br = new BinaryReader(fs))
            {
                // 1. 校验魔数
                int magic = br.ReadInt32();
                if (magic != 0x55AA55AA)
                {
                    throw new Exception("文件格式错误！请重新运行 C++ 生成 mft.dat");
                }

                // 2. 读取数量
                int count = br.ReadInt32();

                for (int i = 0; i < count; i++)
                {
                    // 3. 逐字段读取
                    long id = br.ReadInt64();
                    long parentId = br.ReadInt64();
                    long size = br.ReadInt64();
                    uint attributes = br.ReadUInt32();

                    int nameLen = br.ReadInt32();
                    string name = "";
                    if (nameLen > 0)
                    {
                        byte[] nameBytes = br.ReadBytes(nameLen * 2);
                        name = Encoding.Unicode.GetString(nameBytes);
                    }

                    // 4. 过滤器
                    if (size < 0 || size > 200L * 1024 * 1024 * 1024 * 1024) size = 0;

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

            // --- 树结构组装 ---

            // 确保 C 盘根节点存在 (ID 5)
            if (!nodeLookup.ContainsKey(5))
            {
                nodeLookup[5] = new FileNode { ID = 5, Name = "C:", IsDirectory = true };
            }

            var roots = new List<FileNode>();

            foreach (var kvp in nodeLookup)
            {
                long id = kvp.Key;
                FileNode node = kvp.Value;

                if (id == 5) continue;

                if (parentLookup.TryGetValue(id, out long parentId))
                {
                    if (nodeLookup.TryGetValue(parentId, out FileNode parent))
                    {
                        parent.Children.Add(node);
                        // 【新增】建立父子双向链接
                        node.Parent = parent;
                    }
                    else
                    {
                        // 孤儿 -> 挂给 ID 5
                        nodeLookup[5].Children.Add(node);
                        node.Parent = nodeLookup[5]; // 【新增】
                    }
                }
                else
                {
                    // 无父节点 -> 挂给 ID 5
                    nodeLookup[5].Children.Add(node);
                    node.Parent = nodeLookup[5]; // 【新增】
                }
            }

            // 最后把 ID 5 放入结果列表
            roots.Add(nodeLookup[5]);

            // 递归计算大小
            foreach (var root in roots) CalculateDirectorySize(root);

            return roots;
        }

        private static long CalculateDirectorySize(FileNode node)
        {
            if (!node.IsDirectory) return node.Size;
            long total = 0;
            if (node.Children != null)
            {
                foreach (var child in node.Children)
                {
                    total += CalculateDirectorySize(child);
                }
            }
            node.Size = total;
            return total;
        }
    }
}