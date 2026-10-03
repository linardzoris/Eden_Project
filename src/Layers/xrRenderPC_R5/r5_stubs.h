#pragma once

// EntryPoint 需要完整类型定义才能做隐式向上转换。
// 这些 stub 类在 r5.cpp 中定义，这里通过包含 r5.cpp 不可行，
// 改为在 EntryPoint.cpp 中直接包含 r5.cpp 的声明部分不可行。
// 正确做法：EntryPoint.cpp 不直接引用 stub 类型，而是通过基类指针获取。
// 因此此头文件仅保留前向声明，EntryPoint.cpp 中用 reinterpret_cast。

class dx5RenderFactory;
class dx5UIRender;
class dx5DUInterface;
#ifdef DEBUG_DRAW
class dx5DebugRender;
#endif

extern dx5RenderFactory RenderFactoryImpl;
extern dx5UIRender UIRenderImpl;
extern dx5DUInterface DUImpl;
#ifdef DEBUG_DRAW
extern dx5DebugRender DebugRenderImpl;
#endif
