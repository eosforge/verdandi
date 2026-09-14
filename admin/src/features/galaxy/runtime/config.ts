export const sceneConfig = {
  background: 0x050914,
  targetFps: 60,
  maxPixelRatio: 1.5,
  // 可用恒星统一以一分钟一圈自转, 单位弧度/秒; 调用方可按节点覆盖或设为零停转.
  starRotationRadiansPerSecond: (Math.PI * 2) / 60,
  camera: {
    fieldOfView: 45,
    near: 0.1,
    far: 1600,
    minDistance: 8,
    maxDistance: 440,
    starDistance: 90,
    planetDistance: 18,
    transitionSeconds: 1.2,
    zoomResponseSeconds: 0.09,
    // 拖动响应时间常数为 90 ms; 旋转和平移倍率适当降低, 便于近景细调.
    dragResponseSeconds: 0.09,
    rotateSpeed: 0.65,
    panSpeed: 0.8,
    overviewPosition: [0, 112, 205],
    overviewTarget: [0, 0, 16],
  },
} as const;
