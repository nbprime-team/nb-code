# 与 Prime 交互的关键写法（读源码整理）

> ⚠️ AI 生成 / 辅助创作：DeepSeek V4.1 Flash（未人工审查）

这份笔记记录 nbCode 在真机上反复踩坑后固化下来的写法。改动这些地方前请先读完。

## 1. 启动契约：入口参数只有一个字符串

- `nb_entry` 收到 **r0 = 应用目录的 UTF-8 路径**（或 0）；其余参数一律走
  `nbCode.hpappdir\nbcode.cfg` 文件（`src/nb_cfg.h`）。
- **不要用内存结构体传配置**：launcher 与 C 的结构体布局曾多次不一致，
  表现统一是黑屏 + 日志 "no config block"。文件没有布局，短读/魔数错误可检测。
- 入口必须**保存 callee-saved 寄存器**（r4–r11, ip, lr）并**把 SP 对齐到 8**：
  ARM926 对非对齐 LDRD/STRD 触发 data abort → 复位（`port/nb_svc.s`）。

## 2. 固件 SVC 调用约定

```
push {r0}
push {lr}
svc  <id>
```

固件通过压栈的 r0 找参数块，用压栈的 LR 返回；对调用者等价于 AAPCS。
全部 SVC 包装集中在 `port/nb_svc.s`。

## 3. 输入钩子：改固件函数指针必须做 CP15 仪式

- 固件通过槽位 **0x307FBFA0** 调 `get_event`。写入
  `{ldr pc,[pc,#-4]; <addr>}` 后所有事件进入本映像。
- 钩子在**系统输入线程**上解码并排队；不要假设必须"只拷贝原始事件"。
- **写固件代码的 CP15 顺序**（`nb_slot_patch`，抄自 PureDOOM）：
  关 IRQ → 保存 TTBR → 用 `mvn r5,#0` 写 TTBR 让核静默 → 写两字 →
  clean+invalidate D-cache → invalidate I-cache → drain write buffer → 恢复 TTBR。
  **顺序错了会复位**，primeLua 为此丢过两次会话。
- 退出时**必须解除钩子**（`armed=0`）；launcher 释放映像前会检查
  `slot_armed()`，只有"槽位已不指向本映像且不再忙"才允许复用内存块。

## 4. 屏幕：不要猜 framebuffer

```
p  = get_lcd();              // svc 0x1008D
q  = *(u32 *)p;              // info 结构
w  = *(u16 *)(q + 2);
h  = *(u16 *)(q + 4);
fb = *(u32 *)(q + 16);       // 像素，0x00RRGGBB
```

- 曾把 `p[0]` 当像素地址 → 每帧写进固件内存 → 第一帧就复位。
- `nb_screen_init()` **任何一步不成立就返回 0**，宁可不启动也不猜。
- 绘制到 **shadow buffer**，再一次性拷贝到固件缓冲：避免"先擦后画"的闪屏；
  分配失败仍可工作（只是闪），并写日志。

## 5. 自描述与可诊断

- 映像里放魔数结构：`NBINPUT`（输入状态，launcher 可读 `armed` 并解除）、
  `NBCORING`（输出环）。launcher 用 `find_magic` 从映像里找地址，不写死。
- 配置/版本/退出状态都写日志；黑屏时先看日志最后一行。

## 6. launcher 的约束

- MicroPython 的文件 API **拒绝绝对路径**；固件 `fopen` **需要绝对路径**。
  所以目录路径由 launcher 从 r0 交给映像，映像自己拼绝对路径。
- launcher 不要依赖 `seek()`/`flush()`（真机上不可靠）。
- 复用上一次的映像块前，必须同时满足：无未完成调用 + 输入槽已解除。

## 7. 构建

`-ffreestanding -fPIC -fno-builtin -fno-strict-aliasing`、
`-ffunction-sections -fdata-sections` + `--gc-sections`；
不链接 libc，字符串/堆/字体都在映像内。字体 360 KB 只保留可达字形。
