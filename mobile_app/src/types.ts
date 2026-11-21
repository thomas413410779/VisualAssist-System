export type CameraConfig = {
  baseUrl: string;
};

export type GeminiDescription = {
  text: string;
  generatedAt: number;
};

export type ChestTelemetry = {
  ultrasonicCm: number;
  emergencyBrake: boolean;
  connected: boolean;
  lastUpdated: number;
  sonarHealthy: boolean;
  sonarTimeouts: number;
};

export const DEFAULT_CHEST_TELEMETRY: ChestTelemetry = {
  ultrasonicCm: 400,
  emergencyBrake: false,
  connected: false,
  lastUpdated: 0,
  sonarHealthy: true,
  sonarTimeouts: 0,
};
