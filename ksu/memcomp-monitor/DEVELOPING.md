# Memcomp Monitor

安装包的模块 ID 为 `gt5_memcomp_monitor`。内存监控使用原有 `snapshot.sh`。
首页的压缩模式通过 KSU bridge 运行 `sh compressor.sh status`、`sh compressor.sh set lz4` 或
`sh compressor.sh set zstd`。固定端点为 `/sys/kernel/mm/memcomp/compressor`。
脚本读取真实权限位，在每次设置前复核能力，设置后重新读取算法和权限。
只读、未知模式、异常返回或写入失败时，页面不会报告切换成功。失败后页面再次查询实际值。
不会保存选择或在开机脚本中写入算法。压缩历史页的解码由支持混合算法的内核负责。

## 离线验证

在具备 Node.js、playwright-core 和该版本配套 Chromium 的电脑上运行：

```sh
node tools/verify.cjs
```

若 playwright-core 安装在其他位置，可通过 `PLAYWRIGHT_CORE` 指定其模块目录。仅运行
shell 夹具时可执行 `node tools/verify.cjs --shell-only`。测试在临时目录内复制脚本并替换
唯一的 sysfs 路径，再模拟权限、写入错误和回读异常。浏览器打开本地 WebUI，模拟 KSU
回调并执行夹具脚本。驱动修复页的测试模拟检查、写入回执和复查结果。夹具只验证模块协议和
交互，不执行内核压缩或解压。

## 构建

```sh
python3 tools/package.py /path/to/output
```

输出同一版本的安装 ZIP 和源码 ZIP。将安装 ZIP 交给 KernelSU 管理器；
源码 ZIP 额外包含本说明、构建脚本和离线验证脚本。构建时按脚本中的清单打包，
模块文件直接置于 ZIP 根目录，固定文件权限与 ZIP 时间戳。
