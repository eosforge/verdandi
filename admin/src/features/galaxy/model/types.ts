export type Position3 = readonly [number, number, number];
export type PlanetKind = "Registry" | "Subscriber" | "Publisher";
export type StarStatus = "available" | "unavailable";

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
  /** 展示快照声明的可用性, 不由渲染器根据颜色或实体数量推断. */
  readonly status: StarStatus;
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
  readonly sourceLabel: string;
  readonly description: string;
}

export type GalaxySelection = { readonly star: Star; readonly planet: Planet | null } | null;

/** 公开控制器仅接受稳定标识; 行星操作要求已进入所属星系, 不满足条件或已释放时返回 false. */
export interface GalaxyController {
  selectStar(starId: string): boolean;
  /** 设置恒星绕本地 Y 轴自转的弧度/秒, 默认 2π/60; 接受有限正负值, 0 停转, 不可用节点或无效输入返回 false. */
  setStarRotationSpeed(starId: string, radiansPerSecond: number): boolean;
  selectPlanet(starId: string, planetId: string): boolean;
  focusPlanet(starId: string, planetId: string): boolean;
  overview(): void;
  dispose(): void;
}

export interface GalaxyCallbacks {
  select(selection: GalaxySelection): void;
  fps(value: number): void;
  error(message: string): void;
}
