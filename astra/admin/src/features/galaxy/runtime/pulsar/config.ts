// 脉冲星的局部模型尺度与展示速度; 可见光束和时间均为艺术化表达, 不是观测标定.
export const pulsarConfig = {
  // 原始球面半径; 装配时沿用恒星数量缩放规则.
  coreRadius: 3.6,
  // 光束体积沿每个磁极延伸的总长度与包围半径, 包含末端淡出区.
  beamLength: 42,
  beamRadius: 6,
  // 偶极磁场示意线的赤道半径.
  fieldRadius: 15,
  // 核心周围的体积辉光包围半径, 发射在此边界前平滑归零.
  haloRadius: 12,
  // 光束与磁场细丝的周期性流动时长, 独立于自转速度与行星暂停.
  flowSeconds: 4,
  // 磁轴相对自转轴倾斜 30 度; 自转轴相对世界 Y 轴也倾斜 30 度.
  magneticTilt: Math.PI / 6,
  spinTilt: Math.PI / 6,
  // 默认每 2.4 秒一圈; 通过现有节点自转接口可停转、反转或改速.
  radiansPerSecond: (Math.PI * 2) / 2.4,
} as const;
