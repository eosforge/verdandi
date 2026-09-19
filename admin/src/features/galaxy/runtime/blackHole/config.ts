// 黑洞局部模型坐标、光路表范围与采样精度; CPU 预计算和 GPU 共用.
export const blackHoleOptics = {
  // 临界冲量对应的阴影半径, 与 Schwarzschild 半径满足 3√3/2 的比例.
  shadowRadius: 4.5,
  // 事件视界的局部模型半径; 轨道积分只接受正值.
  schwarzschildRadius: Math.sqrt(3),
  // 查找表最小冲量, 保持正值以避开原点奇点.
  minImpact: 0.02,
  // 查找表最大冲量, 同时限制光学体积的有效成像范围.
  maxImpact: 28,
  // GLB 光学包围网格半径, 必须覆盖盘面和有效冲量范围.
  volumeRadius: 30,
  // 吸积盘内缘半径, 非旋转模型的最内稳定圆轨道为三个视界半径.
  discInner: Math.sqrt(3) * 3,
  // 吸积盘外缘半径, 与离线模型保持一致.
  discOuter: 24,
  // 外缘发射开始衰减的半径, 位于内外缘之间.
  discFadeStart: 19.5,
  // 前景盘层的半厚度, 用于近侧视时的局部体积采样.
  discHalfHeight: 0.28,
  // 光轨道最大积分角, 单位弧度; 当前覆盖前两次盘面交点.
  maxAngle: Math.PI * 3,
  // 轨道表的冲量采样列数, 奇数确保临界冲量有独立中间列.
  width: 513,
  // 轨道表角度采样行数, 整数且至少为 2.
  height: 768,
  // 有限距离观察者相位表的逆距离采样行数, 整数且至少为 2.
  observerHeight: 128,
  // 观察者相位表覆盖的最小局部半径; 更近位置钳制到首个距离边界.
  observerMinRadius: 6,
} as const;
