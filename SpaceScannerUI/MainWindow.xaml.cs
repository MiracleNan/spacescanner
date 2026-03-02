using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;

namespace MftScanner
{
    public partial class MainWindow : Window
    {
        private List<FileNode>? _currentRoots;
        private const int DefaultMaxDepth = 4;
        private const string CppEngineName = "MftEngine.exe";
        private const string TempResultFile = "scan_result.dat";
        private readonly HashSet<FileNode> _expandedNodes = new HashSet<FileNode>();

        // 【双击检测变量】
        private long _lastClickTicks = 0;
        private FileNode? _lastClickNode = null;

        public MainWindow()
        {
            InitializeComponent();
            Loaded += MainWindow_Loaded;
        }

        private void MainWindow_Loaded(object sender, RoutedEventArgs e)
        {
            var drives = DriveInfo.GetDrives()
                .Where(d => d.IsReady && d.DriveType == DriveType.Fixed)
                .Select(d => d.Name.Replace("\\", ""))
                .ToList();

            DriveSelector.ItemsSource = drives;
            if (drives.Count > 0) DriveSelector.SelectedIndex = 0;
        }

        // --- 核心功能：释放引擎 ---
        private string ExtractEngine()
        {
            string extractDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "SpaceScanner");
            Directory.CreateDirectory(extractDir);
            string destPath = System.IO.Path.Combine(extractDir, CppEngineName);

            try
            {
                var assembly = System.Reflection.Assembly.GetExecutingAssembly();
                string resourceName = $"{assembly.GetName().Name}.MftEngine.exe";
                if (!assembly.GetManifestResourceNames().Contains(resourceName))
                {
                    resourceName = assembly.GetManifestResourceNames()
                        .FirstOrDefault(n => n.EndsWith(".MftEngine.exe", StringComparison.OrdinalIgnoreCase))
                        ?? resourceName;
                }

                using (Stream? stream = assembly.GetManifestResourceStream(resourceName))
                {
                    if (stream == null)
                    {
                        string[] allResources = assembly.GetManifestResourceNames();
                        throw new Exception($"无法找到嵌入资源。\n现有资源:\n{string.Join("\n", allResources)}");
                    }
                    using (FileStream fileStream = new FileStream(destPath, FileMode.Create, FileAccess.Write))
                    {
                        stream.CopyTo(fileStream);
                    }
                }
            }
            catch (Exception ex) { throw new Exception("引擎释放失败: " + ex.Message); }
            return destPath;
        }

        private string RunCppEngine(string exePath, string drive, string outputPath)
        {
            var startInfo = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = $"{drive} \"{outputPath}\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            using (var process = Process.Start(startInfo))
            {
                if (process == null) throw new Exception("无法启动引擎");
                string stdout = process.StandardOutput.ReadToEnd();
                string stderr = process.StandardError.ReadToEnd();
                if (!process.WaitForExit(10 * 60 * 1000))
                {
                    process.Kill(true);
                    throw new Exception("扫描超时（超过 10 分钟）");
                }
                if (process.ExitCode != 0) throw new Exception($"扫描引擎异常退出 (Code: {process.ExitCode})\n{stderr}\n{stdout}");
                return stdout;
            }
        }

        private async void BtnAnalyze_Click(object sender, RoutedEventArgs e)
        {
            string? selectedDrive = DriveSelector.SelectedItem as string;
            if (string.IsNullOrEmpty(selectedDrive)) return;

            string enginePath;
            try { enginePath = ExtractEngine(); }
            catch (Exception ex) { MessageBox.Show(ex.Message); return; }

            BtnAnalyze.IsEnabled = false;
            StatusText.Text = $"正在扫描 {selectedDrive}...";
            TreemapCanvas.Children.Clear();

            var sw = Stopwatch.StartNew();
            string tempFile = System.IO.Path.Combine(System.IO.Path.GetTempPath(), TempResultFile);
            string engineOutput = "";
            try
            {
                if (File.Exists(tempFile)) File.Delete(tempFile);
                engineOutput = await Task.Run(() => RunCppEngine(enginePath, selectedDrive, tempFile));
                if (!File.Exists(tempFile))
                    throw new Exception("扫描结果文件未生成，请确认以管理员权限运行。");

                StatusText.Text = "正在解析数据...";
                var roots = await Task.Run(() => MftParser.Parse(tempFile));

                var rootNode = roots.FirstOrDefault(n => n.ID == 5)
                             ?? roots.OrderByDescending(n => n.Size).FirstOrDefault();

                sw.Stop();
                if (rootNode != null)
                {
                    _currentRoots = new List<FileNode> { rootNode };
                    _expandedNodes.Clear();
                    string engineMode = "Unknown";
                    if (engineOutput.Contains("[Mode] DataRuns", StringComparison.OrdinalIgnoreCase))
                        engineMode = "DataRuns";
                    else if (engineOutput.Contains("[Mode] RecordIoctlFallback", StringComparison.OrdinalIgnoreCase))
                        engineMode = "RecordIoctlFallback";

                    StatusText.Text = $"分析完毕 | 模式: {engineMode} | 耗时: {sw.Elapsed.TotalSeconds:F2}s | 已用空间: {FormatSize(rootNode.Size)}";
                    DrawTreemap();
                }
                else
                {
                    StatusText.Text = "未找到有效数据";
                }
            }
            catch (Exception ex) { MessageBox.Show(ex.Message); StatusText.Text = "失败"; }
            finally
            {
                if (File.Exists(tempFile)) File.Delete(tempFile);
                BtnAnalyze.IsEnabled = true;
            }
        }

        private void TreemapCanvas_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (_currentRoots != null) DrawTreemap();
        }

        private void DrawTreemap()
        {
            TreemapCanvas.Children.Clear();
            if (_currentRoots == null || _currentRoots.Count == 0) return;
            DrawNodeRecursive(_currentRoots[0], new Rect(0, 0, TreemapCanvas.ActualWidth, TreemapCanvas.ActualHeight), 0);
        }

        // --- 逻辑双击处理 ---
        private void HandleDoubleClick(FileNode node)
        {
            if (_currentRoots == null || _currentRoots.Count == 0) return;

            // 逻辑 1: 如果双击的是当前的根节点 -> 返回上一级 (Exit)
            if (node == _currentRoots[0])
            {
                if (node.Parent != null)
                {
                    _currentRoots = new List<FileNode> { node.Parent };
                    // 返回上一级时，自动把当前这级设为展开，方便查看
                    if (!_expandedNodes.Contains(node.Parent)) _expandedNodes.Add(node.Parent);
                }
            }
            // 逻辑 2: 如果双击的是里面的子节点 -> 钻取进入 (Drill Down)
            else
            {
                _currentRoots = new List<FileNode> { node };
                // 钻取进去后，默认把新根节点设为展开，否则进去只能看到一个大色块
                if (!_expandedNodes.Contains(node)) _expandedNodes.Add(node);
            }

            // 更新标题栏提示
            var current = _currentRoots[0];
            string pathName = current.Name ?? "";
            // 【修改】移除了状态栏里的提示文字，保持界面整洁
            StatusText.Text = $"当前视图: {pathName} | 大小: {FormatSize(current.Size)}";

            DrawTreemap();
        }

        private void DrawNodeRecursive(FileNode node, Rect rect, int depth)
        {
            if (rect.Width < 2 || rect.Height < 2) return;

            bool isExpanded = depth < DefaultMaxDepth || _expandedNodes.Contains(node);
            bool isContainer = node.IsDirectory && node.Children.Count > 0 && isExpanded;

            var border = new Border
            {
                Width = rect.Width,
                Height = rect.Height,
                BorderBrush = Brushes.Black,
                BorderThickness = new Thickness(0.5),
                Background = GetSpaceSnifferColor(node, depth),
                Padding = new Thickness(1),
                Cursor = node.IsDirectory ? Cursors.Hand : Cursors.Arrow,
                Tag = node
            };

            // --- 右键菜单 ---
            var contextMenu = new ContextMenu();
            border.ContextMenuOpening += (s, args) =>
            {
                contextMenu.Items.Clear();
                string fullPath = GetFullPath(node);
                if (node.IsDirectory)
                {
                    var itemOpen = new MenuItem { Header = "在资源管理器中打开" };
                    itemOpen.Click += (sender, e) => OpenPath(fullPath);
                    contextMenu.Items.Add(itemOpen);
                }
                else
                {
                    var itemOpen = new MenuItem { Header = "打开 (默认)" };
                    itemOpen.Click += (sender, e) => OpenPath(fullPath);
                    contextMenu.Items.Add(itemOpen);
                    var itemLocate = new MenuItem { Header = "打开所在的文件夹" };
                    itemLocate.Click += (sender, e) => OpenPath(fullPath, selectFile: true);
                    contextMenu.Items.Add(itemLocate);
                }
            };
            border.ContextMenu = contextMenu;

            Canvas.SetLeft(border, rect.X);
            Canvas.SetTop(border, rect.Y);

            // --- 核心：点击事件 (单击展开/双击钻取) ---
            border.MouseLeftButtonDown += (s, e) =>
            {
                if (node.IsDirectory)
                {
                    long now = DateTime.Now.Ticks;
                    // 判断双击：同一个节点，且间隔小于 500ms (5000000 ticks)
                    if (node == _lastClickNode && (now - _lastClickTicks) < 5000000)
                    {
                        // === 触发双击逻辑 ===
                        HandleDoubleClick(node);
                        _lastClickNode = null; // 重置防止三击触发
                    }
                    else
                    {
                        // === 触发单击逻辑 (展开/折叠) ===
                        if (_expandedNodes.Contains(node))
                            _expandedNodes.Remove(node);
                        else
                            _expandedNodes.Add(node);

                        DrawTreemap();

                        // 记录本次点击，供下次比对
                        _lastClickNode = node;
                        _lastClickTicks = now;
                    }
                    e.Handled = true;
                }
            };

            // 文字显示
            if (rect.Width > 35 && rect.Height > 20)
            {
                string nodeName = node.Name ?? "Unknown";
                string sizeText = FormatSize(node.Size);
                string displayText = isContainer ? $"{nodeName} - {sizeText}" : $"{nodeName}\n{sizeText}";

                var textBlock = new TextBlock
                {
                    Text = displayText,
                    Foreground = Brushes.White,
                    FontSize = isContainer ? 10 : 10.5,
                    FontWeight = FontWeights.Normal,
                    TextTrimming = TextTrimming.CharacterEllipsis,
                    TextWrapping = TextWrapping.Wrap,
                    VerticalAlignment = isContainer ? VerticalAlignment.Top : VerticalAlignment.Center,
                    HorizontalAlignment = isContainer ? HorizontalAlignment.Left : HorizontalAlignment.Center,
                    TextAlignment = isContainer ? TextAlignment.Left : TextAlignment.Center,
                    Margin = isContainer ? new Thickness(3, 1, 3, 0) : new Thickness(0),
                    IsHitTestVisible = false
                };
                border.Child = textBlock;
            }

            // 悬停效果
            border.MouseEnter += (s, e) =>
            {
                border.BorderBrush = Brushes.Yellow;
                border.BorderThickness = new Thickness(2);
                if (InfoPopup != null && PopupText != null)
                {
                    InfoPopup.IsOpen = true;

                    // 【修改】构建更智能的提示信息
                    string typeStr = node.IsDirectory ? "文件夹" : "文件";
                    string hint = "";

                    if (node.IsDirectory)
                    {
                        // 判断该节点是否是当前视图的“根” (即充满整个画布的那个)
                        bool isViewRoot = (_currentRoots != null && _currentRoots.Count > 0 && node == _currentRoots[0]);

                        if (isViewRoot)
                        {
                            // 如果是视图根，且它还有父级，说明可以返回
                            if (node.Parent != null)
                                hint = "\n★ 双击背景返回上一级";
                            else
                                hint = "\n(已是磁盘根目录)";
                        }
                        else
                        {
                            // 普通子文件夹
                            string clickAction = (!isContainer) ? "展开" : "折叠";
                            hint = $"\n• 单击{clickAction}\n• 双击聚焦此目录";
                        }
                    }

                    PopupText.Text = $"名称: {node.Name}\n大小: {FormatSize(node.Size)}\n类型: {typeStr}{hint}";
                }
            };
            border.MouseLeave += (s, e) =>
            {
                border.BorderBrush = Brushes.Black;
                border.BorderThickness = new Thickness(0.5);
                if (InfoPopup != null) InfoPopup.IsOpen = false;
            };

            TreemapCanvas.Children.Add(border);

            // 递归
            if (isContainer)
            {
                double margin = Math.Max(1.0, 3.0 - depth * 0.5);
                double headerHeight = 15.0;
                double contentX = rect.X + margin;
                double contentY = rect.Y + margin + headerHeight;
                double contentW = Math.Max(0, rect.Width - margin * 2);
                double contentH = Math.Max(0, rect.Height - margin * 2 - headerHeight);

                if (contentW <= 1 || contentH <= 1) return;

                Rect contentRect = new Rect(contentX, contentY, contentW, contentH);
                var children = node.Children.OrderByDescending(c => c.Size).ToList();
                Squarify(children, contentRect, depth + 1);
            }
        }

        // --- 辅助方法 ---
        private string GetFullPath(FileNode node)
        {
            var parts = new Stack<string>();
            var current = node;
            while (current != null)
            {
                string name = current.Name ?? "";
                if (current.Parent == null)
                {
                    if (name == "." || name == "")
                    {
                        if (DriveSelector.SelectedItem is string drive) name = drive;
                        else name = "C:";
                    }
                }
                parts.Push(name);
                current = current.Parent;
            }
            string fullPath = string.Join("\\", parts);
            if (fullPath.Length == 2 && fullPath[1] == ':') fullPath += "\\";
            else if (fullPath.Length > 2 && fullPath[1] == ':' && fullPath[2] != '\\') fullPath = fullPath.Insert(2, "\\");
            return fullPath;
        }

        private void OpenPath(string path, bool selectFile = false)
        {
            try
            {
                if (selectFile) Process.Start("explorer.exe", $"/select,\"{path}\"");
                else Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
            }
            catch (Exception ex) { MessageBox.Show($"无法打开: {path}\n{ex.Message}"); }
        }

        private void Squarify(List<FileNode> children, Rect rect, int depth)
        {
            if (children.Count == 0 || rect.Width <= 0 || rect.Height <= 0) return;
            double totalSize = children.Sum(c => c.Size);
            if (totalSize <= 0) return;
            double totalArea = rect.Width * rect.Height;
            double sizeToAreaFactor = totalArea / totalSize;
            var currentRow = new List<FileNode>();
            double currentX = rect.X;
            double currentY = rect.Y;
            double containerW = rect.Width;
            double containerH = rect.Height;

            foreach (var child in children)
            {
                if (containerW <= 0 || containerH <= 0) break;
                if (currentRow.Count > 0)
                {
                    double sideLength = Math.Min(containerW, containerH);
                    double worstRatioWithChild = CalculateWorstRatio(currentRow, child, sideLength, sizeToAreaFactor);
                    double worstRatioWithoutChild = CalculateWorstRatio(currentRow, null, sideLength, sizeToAreaFactor);
                    if (worstRatioWithChild > worstRatioWithoutChild)
                    {
                        LayoutRow(currentRow, ref currentX, ref currentY, ref containerW, ref containerH, depth, sizeToAreaFactor);
                        currentRow.Clear();
                    }
                }
                currentRow.Add(child);
            }
            if (currentRow.Count > 0) LayoutRow(currentRow, ref currentX, ref currentY, ref containerW, ref containerH, depth, sizeToAreaFactor);
        }

        private void LayoutRow(List<FileNode> row, ref double x, ref double y, ref double w, ref double h, int depth, double factor)
        {
            if (row.Count == 0) return;
            double rowArea = row.Sum(c => c.Size) * factor;
            bool containerIsTall = (w < h);
            double sideLength = containerIsTall ? w : h;
            double rowThickness = (sideLength > 0) ? (rowArea / sideLength) : 0;
            if (double.IsNaN(rowThickness) || double.IsInfinity(rowThickness)) rowThickness = 0;

            double currentX = x;
            double currentY = y;

            foreach (var node in row)
            {
                double nodeArea = node.Size * factor;
                if (containerIsTall)
                {
                    double nodeH = rowThickness;
                    double nodeW = (rowThickness > 0) ? (nodeArea / rowThickness) : 0;
                    if (nodeW > 0 && nodeH > 0) DrawNodeRecursive(node, new Rect(currentX, y, nodeW, nodeH), depth);
                    currentX += nodeW;
                }
                else
                {
                    double nodeW = rowThickness;
                    double nodeH = (rowThickness > 0) ? (nodeArea / rowThickness) : 0;
                    if (nodeW > 0 && nodeH > 0) DrawNodeRecursive(node, new Rect(x, currentY, nodeW, nodeH), depth);
                    currentY += nodeH;
                }
            }
            if (containerIsTall) { y += rowThickness; h = Math.Max(0, h - rowThickness); }
            else { x += rowThickness; w = Math.Max(0, w - rowThickness); }
        }

        private double CalculateWorstRatio(List<FileNode> row, FileNode? newChild, double sideLength, double factor)
        {
            if (sideLength <= 0.0001) return double.MaxValue;
            double minSize = row.Min(c => c.Size);
            double maxSize = row.Max(c => c.Size);
            double sumSize = row.Sum(c => c.Size);
            if (newChild != null)
            {
                minSize = Math.Min(minSize, newChild.Size);
                maxSize = Math.Max(maxSize, newChild.Size);
                sumSize += newChild.Size;
            }
            double minArea = minSize * factor;
            double maxArea = maxSize * factor;
            double sumArea = sumSize * factor;
            if (minArea <= 0 || sumArea <= 0) return double.MaxValue;
            double s2 = sideLength * sideLength;
            double sum2 = sumArea * sumArea;
            return Math.Max((s2 * maxArea) / sum2, sum2 / (s2 * minArea));
        }

        private Brush GetSpaceSnifferColor(FileNode node, int depth)
        {
            double hue, saturation, value;
            if (node.IsDirectory)
            {
                hue = 35;
                saturation = 0.55 - (depth * 0.04);
                value = 0.96 - (depth * 0.07);
                bool isExpanded = depth < DefaultMaxDepth || _expandedNodes.Contains(node);
                if (!isExpanded) { hue = 32; saturation = 0.65; value = 0.65; }
            }
            else
            {
                hue = 210; saturation = 0.35;
                int hash = Math.Abs(node.Name?.GetHashCode() ?? 0);
                hue += (hash % 8) - 4;
                value = 0.92 - (depth * 0.08);
            }
            saturation = Math.Clamp(saturation, 0.1, 1.0);
            value = Math.Clamp(value, 0.3, 1.0);
            return new SolidColorBrush(ColorFromHSV(hue, saturation, value));
        }

        private static Color ColorFromHSV(double hue, double saturation, double value)
        {
            int hi = Convert.ToInt32(Math.Floor(hue / 60)) % 6;
            double f = hue / 60 - Math.Floor(hue / 60);
            value = value * 255;
            byte v = Convert.ToByte(value);
            byte p = Convert.ToByte(value * (1 - saturation));
            byte q = Convert.ToByte(value * (1 - f * saturation));
            byte t = Convert.ToByte(value * (1 - (1 - f) * saturation));
            if (hi == 0) return Color.FromRgb(v, t, p);
            else if (hi == 1) return Color.FromRgb(q, v, p);
            else if (hi == 2) return Color.FromRgb(p, v, t);
            else if (hi == 3) return Color.FromRgb(p, q, v);
            else if (hi == 4) return Color.FromRgb(t, p, v);
            else return Color.FromRgb(v, p, q);
        }

        private string FormatSize(long bytes)
        {
            string[] sizes = { "B", "KB", "MB", "GB", "TB" };
            double len = bytes;
            int order = 0;
            while (len >= 1024 && order < sizes.Length - 1) { order++; len /= 1024; }
            return $"{len:0.00} {sizes[order]}";
        }
    }
}
