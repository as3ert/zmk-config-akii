# AKII ZMK 固件配置

Apple Keyboard II 复刻 · XIAO nRF52840 + 2× MCP23017 (0x20 / 0x24)

## 构建方法（推荐：GitHub Actions，零环境配置）

1. 在 GitHub 新建一个仓库（如 `zmk-config-akii`），把 **zmk-config 目录里的全部内容**推上去：

   ```bash
   cd zmk-config
   git init && git add . && git commit -m "AKII initial config"
   git remote add origin git@github.com:<你的用户名>/zmk-config-akii.git
   git push -u origin main
   ```

2. 推送后 GitHub 会自动开始构建（Actions 标签页可看进度，首次约 5-10 分钟）
3. 构建完成后在 Actions 的 run 页面下载 **firmware** 工件,解压得到 `akii-seeeduino_xiao_ble-zmk.uf2`
4. XIAO 双击复位进 bootloader,把 .uf2 拖进弹出的 U 盘——完成,系统里会出现名为 "AKII" 的键盘（USB + 蓝牙）

## 键位

- 基础层：原版 Apple Keyboard II 布局,数字键盘齐全
- **按住数字键盘左上角 CLEAR 键 = Fn 层**：
  - `~` 位置 = 进 bootloader（刷固件不用拆壳按复位）
  - 数字 1-5 = 切换蓝牙配对槽 1-5；6 = 强制 USB 输出；7 = 强制蓝牙；0 = 清除当前配对
- 编码器：旋转 = 音量,按下 = 静音；Fn 层旋转 = 屏幕亮度

## 已知限制

- 矩阵走 I²C 扩展器轮询（无 INT 线）,深度睡眠未开启——电池待机会偏短,USB 使用无影响。飞线接通 INT 后可改造为中断唤醒
- 若编码器一格跳两步或两格跳一步,调整 overlay 里的 `steps` / `triggers-per-rotation`

## 排错

- 构建报错 `microchip,mcp23017` 找不到：说明所用 Zephyr 版本驱动兼容串不同,把 overlay 中两处 compatible 换成 `"microchip,mcp230xx"` 重试
- 全部按键无响应：检查 `akii.conf` 的 POLLING 两项是否生效（构建日志搜 KSCAN）
- 单键/单列异常：硬件层面排查思路见仓库根目录 `project_context.md`
