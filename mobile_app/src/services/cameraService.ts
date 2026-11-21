const normalizeBaseUrl = (baseUrl: string): string => baseUrl.trim().replace(/\/$/, '');

export const cameraStreamUrl = (baseUrl: string): string => {
  const url = normalizeBaseUrl(baseUrl);
  if (!url) return '';
  try {
    const parsed = new URL(url);
    if (!parsed.port || parsed.port === '80') {
      parsed.port = '81';
      return parsed.toString();
    }
  } catch {
    // fallback
  }
  return `${url}/`;
};

export const cameraCaptureUrl = (baseUrl: string): string => {
  const url = normalizeBaseUrl(baseUrl);
  return `${url}/capture`;
};

export const cameraAudioUrl = (baseUrl: string): string => {
  const url = normalizeBaseUrl(baseUrl);
  return `${url}/audio`;
};

export type ChestStatus = {
  triggerCounter: number;
  audioCounter?: number;
  isRecording?: boolean;
  micReady?: boolean;
  speakerReady?: boolean;
  ip: string;
  ultrasonicCm?: number;
  emergencyBrake?: boolean;
  sonarDurationUs?: number;
  trigPin?: number;
  echoPin?: number;
  sonarTimeouts?: number;
  sonarHealthy?: boolean;
  cameraReady?: boolean;
  cameraModel?: string;
  wifiRssi?: number;
};

export async function captureCameraJpeg(baseUrl: string): Promise<Blob> {
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 4000);
  try {
    const response = await fetch(cameraCaptureUrl(baseUrl), {
      method: 'GET',
      cache: 'no-store',
      signal: controller.signal,
    });
    clearTimeout(timeoutId);
    if (!response.ok) {
      throw new Error(`相機擷取失敗：HTTP ${response.status}`);
    }
    return await response.blob();
  } catch (e) {
    clearTimeout(timeoutId);
    throw e;
  }
}

/**
 * 優先從當前畫面 <img> 元素瞬間快照（0 網路延遲，避免與 ESP32 MJPEG 串流搶佔連線）
 * 若相機當前無法取得影格，優雅回傳 null，避免卡死或中斷使用者文字問答
 */
export async function captureCurrentStreamFrame(source: HTMLImageElement | HTMLVideoElement | null, baseUrl: string): Promise<Blob | null> {
  if (source) {
    const isVideo = source instanceof HTMLVideoElement;
    const width = isVideo ? (source as HTMLVideoElement).videoWidth : (source as HTMLImageElement).naturalWidth;
    const height = isVideo ? (source as HTMLVideoElement).videoHeight : (source as HTMLImageElement).naturalHeight;

    if (width > 0 && height > 0) {
      try {
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        if (ctx) {
          ctx.drawImage(source, 0, 0);
          const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, 'image/jpeg', 0.85));
          if (blob && blob.size > 100) return blob;
        }
      } catch {
        // 若受瀏覽器限制則嘗試網路請求
      }
    }
  }
  if (!baseUrl) return null;
  try {
    return await captureCameraJpeg(baseUrl);
  } catch {
    return null;
  }
}

/**
 * 從胸前板獲取最新錄製的使用者提問音訊 (WAV 格式，含逾時保護)
 */
export async function fetchLatestAudio(baseUrl: string): Promise<Blob | null> {
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 2500);
  try {
    const response = await fetch(cameraAudioUrl(baseUrl), {
      method: 'GET',
      cache: 'no-store',
      signal: controller.signal,
    });
    clearTimeout(timeoutId);
    if (!response.ok) return null;
    return await response.blob();
  } catch {
    clearTimeout(timeoutId);
    return null;
  }
}

/**
 * 將音訊發送至胸前板，由內建 MAX98357 擴大機與喇叭接口播放
 */
export async function playAudioOnChest(baseUrl: string, audioBlob: Blob): Promise<boolean> {
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 15000);
  try {
    const url = normalizeBaseUrl(baseUrl);
    const response = await fetch(`${url}/play`, {
      method: 'POST',
      body: audioBlob,
      headers: { 'Content-Type': audioBlob.type || 'audio/wav' },
      signal: controller.signal,
    });
    clearTimeout(timeoutId);
    return response.ok;
  } catch {
    clearTimeout(timeoutId);
    return false;
  }
}

/**
 * 觸發胸前板內建蜂鳴/提示音 (/beep)
 */
export async function triggerChestBeep(baseUrl: string): Promise<boolean> {
  try {
    const url = normalizeBaseUrl(baseUrl);
    const response = await fetch(`${url}/beep`, {
      method: 'GET',
      cache: 'no-store',
    });
    return response.ok;
  } catch {
    return false;
  }
}

export async function fetchChestStatus(baseUrl: string): Promise<ChestStatus> {
  const url = normalizeBaseUrl(baseUrl);
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 2500);
  try {
    const response = await fetch(`${url}/status`, {
      method: 'GET',
      cache: 'no-store',
      signal: controller.signal,
    });
    clearTimeout(timeoutId);
    if (!response.ok) {
      throw new Error(`相機狀態讀取失敗：HTTP ${response.status}`);
    }
    return await response.json();
  } catch (err) {
    clearTimeout(timeoutId);
    throw err;
  }
}
