export interface WidgetHttp {
  /** Synchronous inside the isolated host; `await` is accepted for source compatibility. */
  getJson(url: string): unknown;
}

export interface WidgetContext<TSettings = Record<string, unknown>> {
  readonly settings: TSettings;
  readonly http: WidgetHttp;
  readonly systemMetrics: Partial<Record<"cpu" | "storage" | "network" | "memory", Record<string, unknown>>>;
  readonly nowUnix: number;
}

export declare function update(ctx: WidgetContext): Promise<Record<string, unknown>> | Record<string, unknown>;
