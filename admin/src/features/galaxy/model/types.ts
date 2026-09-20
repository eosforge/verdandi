export type Position3 = readonly [number, number, number];
export type PlanetKind = "Registry" | "Subscriber" | "Publisher";
/** 恒星展示状态; 黑洞仍是同一个 Star, 不作为独立实体类型. */
export type StarStatus = "available" | "black-hole";
/** 可用节点的星体外观; 不代表后端角色或健康状态. */
export type StarAppearance = "star" | "pulsar";

export interface Planet {
  readonly id: string;
  readonly name: string;
  readonly kind: PlanetKind;
  readonly starId: string;
  /** 相对于所属 Star 的初始布局参考; 防碰撞规划可调整实际起点和轨道, 不改写快照. */
  readonly position: Position3;
}

export interface Star {
  readonly id: string;
  readonly name: string;
  readonly color: string;
  /** 显式展示状态; black-hole 不显示所属行星或关联连线. */
  readonly status: StarStatus;
  /** 缺省为普通恒星; black-hole 状态优先使用黑洞外观. */
  readonly appearance?: StarAppearance;
  /** 星图布局参考中心; 展示时按星系轨道范围统一扩大间距, 原始快照保持不变. */
  readonly position: Position3;
  readonly planets: readonly Planet[];
}

export interface StarLink {
  readonly source: string;
  readonly target: string;
}

/** 一次完整的展示快照. 更新时替换快照, 不原地修改传入数据. */
export interface GalaxyData {
  readonly stars: readonly Star[];
  readonly links: readonly StarLink[];
  /** 可选总览观察中心节点; 构图仍覆盖整张星图, 不隐式计算引力或恒星质量. */
  readonly centerStarId?: string;
  readonly sourceLabel: string;
  readonly description: string;
}

export type GalaxySelection = { readonly star: Star; readonly planet: Planet | null } | null;

/** 公开控制器仅接受稳定标识; 行星操作要求已进入所属星系, 不满足条件或已释放时返回 false. */
export interface GalaxyController {
  selectStar(starId: string): boolean;
  /** 设置绕本地自转轴的弧度/秒, 普通恒星默认 2π/60, 脉冲星默认 2π/2.4; 接受有限正负值, 0 停转. 无效节点或值返回 false. */
  setStarRotationSpeed(starId: string, radiansPerSecond: number): boolean;
  selectPlanet(starId: string, planetId: string): boolean;
  focusPlanet(starId: string, planetId: string): boolean;
  overview(): void;
  dispose(): void;
}

export interface GalaxyCallbacks {
  select(selection: GalaxySelection): void;
  fps(value: number): void;
  /** 世界坐标, 最多每秒十次且仅变化时上报; 供界面读数, 不参与相机控制. */
  cameraPosition?(position: Position3): void;
  error(message: string): void;
}
