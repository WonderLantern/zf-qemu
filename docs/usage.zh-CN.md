# 本地运行

Windows 用户请在 WSL 中执行命令。仓库仅发布源码、合成测试和输入摘要，不附带固件、dump、密钥、私人图片、运行日志或研究记录。

1. 按 README 安装构建依赖，运行 `python3 tools/build.py --jobs 4`。默认仅下载官方 QEMU 源码，生成的构建缓存不会提交。
2. 将 `config.example.json` 复制到本地私有位置，填写 QEMU 和五项输入路径。输入规格见 `inputs.md`。`state_dir` 选择一个较短的 Linux 路径，用于 Unix socket、卡片、日志和运行结果。
3. 运行 `python3 -m zf_qemu verify --config /path/to/local-config.json`：固定固件组件检查长度和 SHA256；本地 `snor_user`、可选 `nand` 只检查文件存在且可读，不计算或绑定其大小、SHA256。
4. 命令行运行：`python3 -m zf_qemu run --config /path/to/local-config.json`，按 Ctrl+C 停止。
5. 网页运行：`python3 -m zf_qemu web --config /path/to/local-config.json --port 8766`，打开 `http://127.0.0.1:8766/` 后点击“启动”。

`inputs.snor_user` 填自己的 Zf 导出的 SNOR_USER 用户区原始镜像，不填 EG、整包固件或未经提取的整片 Flash dump。`inputs.nand` 填兼容模型的模拟 NAND 镜像模板，不填 LI 文件；没有模板时省略这个可选键。两类镜像均保存在本机，具体格式和配置示例见 `inputs.md`。

命令行和网页共用启动实现，均明确加载 LI，默认没有会话时限。网页只监听 localhost。两张空白 FAT32 模拟卡自动生成，上传文件前须停止虚拟机。自定义图片作为传感器输入，不覆盖 LCD/EVF 输出；修改输入后会重启正在运行的虚拟机。

后续修改直接在本仓库创建分支或 worktree，例如 `git worktree add -b codex/my-change ../zf-qemu-my-change main`。修改 `qemu/` 下实际源码并重新构建，不再运行旧研究目录中的补丁脚本。

模拟器仍不完整。合成电池、镜头与传感器只提供功能模型；视频录制、完整 Linux 启动和实机等价性不作为发布承诺。详见 `verification-status.md`。
