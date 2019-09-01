# LiteOS
写一个简单的OS,根据《操作系统真象还原》
## 已经实现的模块

- 引导程序
- 内存分页+虚拟内存
- 多进程/线程(用户进程/内核线程)的调度,任务特权级切换
- 中断处理模块
- 定时器、磁盘、键盘、显卡等驱动程序
- 类ext2的文件系统
- 动态内存的分配与回收(malloc和free)
- 系统调用
- 外部程序的执行(elf文件的解析与加载,不过只能使用os内部的接口)
- 管道的基础实现

## 演示视频
<video id="video" controls="" preload="none" poster="http://img.blog.fandong.me/2017-08-26-Markdown-Advance-Video.jpg">
    <source id="mp4" src="http://img.blog.fandong.me/2017-08-26-Markdown-Advance-Video.mp4" type="video/mp4">
</video>