// 吸积流的展示时间与径向差速, CPU 亮结和 GPU 流纹使用同一组参数; 不是物理时间标定.
export const blackHoleFlow = {
  // 虚拟秒/真实秒, 保持已选定的三倍流速.
  timeScale: 3,
  // 双相平流的虚拟秒周期, 在零权重处复位, 限制累计剪切.
  cycleSeconds: 24,
  // 盘内缘的角速度, 单位弧度/虚拟秒, 外侧按 r^-1.5 降低, 不附加共同匀速分量.
  innerAngularSpeed: 0.34,
  // 盘内缘向内速度, 单位模型长度/虚拟秒; 外侧按 sqrt(r_in/r) 减慢, 仅控制展示平流.
  innerRadialSpeed: 0.12,
  // 稀薄层比主流缓慢, 用相对运动提示层次, 无额外逐帧纹理生成.
  atmosphereSpeedRatio: 0.68,
  // 稀薄层向内汇聚慢于主层, 与角向差速一起表现层间运动.
  atmosphereInflowRatio: 0.65,
} as const;

// 调用方提供正的盘内缘, 亮结与盘面主流保持一致的角速度.
export function flowAngularSpeed(radius: number, innerRadius: number): number {
  return blackHoleFlow.innerAngularSpeed * (innerRadius / Math.max(radius, innerRadius)) ** 1.5;
}

// 从初始半径沿展示流场前进; 到达内缘后停留并交由亮度包络消隐, 不跳到外缘.
export function advanceFlowKnot(initialRadius: number, elapsedSeconds: number, innerRadius: number): { radius: number; angle: number } {
  const start = Math.max(innerRadius, initialRadius);
  const radialStep = 1.5 * blackHoleFlow.innerRadialSpeed * Math.sqrt(innerRadius) * Math.max(0, elapsedSeconds);
  const radius = Math.max(innerRadius, Math.max(innerRadius ** 1.5, start ** 1.5 - radialStep) ** (2 / 3));
  // 沿收缩轨迹积分角速度, 避免用当前角速度乘总时间造成亮结加速跳动.
  const angle = ((blackHoleFlow.innerAngularSpeed * innerRadius) / blackHoleFlow.innerRadialSpeed) * Math.log(start / radius);
  return { radius, angle };
}
