export type WidgetLifecycleState =
  | "collapsed"
  | "expanding"
  | "expanded"
  | "collapsing"
  | "idle"
  | "suspended";

export interface WidgetSnapshotEvent {
  sequence: number;
  updatedAtUnix: number;
  data: Record<string, unknown>;
  settings: Record<string, unknown>;
}

export interface WidgetLifecycleEvent {
  state: WidgetLifecycleState;
  dpi: number;
  width: number;
  height: number;
  visible?: boolean;
  resourceThrottled?: boolean;
  webglAllowed?: boolean;
  continuousAnimationAllowed?: boolean;
}

export interface TaskbarWidgetStorage {
  get<T = unknown>(key: string): Promise<T | null>;
  set(key: string, value: unknown): Promise<void>;
  delete(key: string): Promise<void>;
}

export interface TaskbarWidgetApi {
  readonly widgetId: string;
  readonly instanceId: string;
  readonly storage: TaskbarWidgetStorage;
  ready(): Promise<void>;
  on(event: "snapshot", callback: (event: WidgetSnapshotEvent) => void): () => void;
  on(event: "settings", callback: (settings: Record<string, unknown>) => void): () => void;
  on(event: "lifecycle", callback: (event: WidgetLifecycleEvent) => void): () => void;
  requestSurface(state: "expanded" | "collapsed"): Promise<void>;
  openSettings(): Promise<void>;
  invoke(action: string, args?: unknown): Promise<void>;
}

declare global {
  interface Window {
    taskbarWidget: TaskbarWidgetApi;
  }
}
