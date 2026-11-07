import { useEffect, useRef, useState } from 'react';
import {
  cameraStreamUrl,
  captureCameraJpeg,
  captureCurrentStreamFrame,
  fetchChestStatus,
  fetchLatestAudio,
  triggerChestBeep,
  type ChestStatus,
} from './services/cameraService';
import {
  describeImageWithGemini,
  askGeminiWithImageAndAudio,
  getGeminiApiKey,
  setGeminiApiKey,
} from './services/geminiService';
import { cancelSpeech, playTone, speak, speakResponse } from './services/ttsService';
import {
  LocalYoloEdgeDetector,
  type Detection,
  type SpatialZone,
  type ThreatLevel,
} from './services/edgeDetector';
import {
  DEFAULT_CHEST_TELEMETRY,
  type ChestTelemetry,
} from './types';
import './styles.css';

const CAMERA_URL_STORAGE_KEY = 'visual-assist-camera-url';
const DEFAULT_CAMERA_URL = 'http://visualassist.local';

interface ConversationItem {
  id: string;
  type: 'voice' | 'text' | 'scene';
  question?: string;
  answer: string;
  timestamp: number;
}

function App() {
  const [cameraUrl, setCameraUrl] = useState(() => {
    const saved = localStorage.getItem(CAMERA_URL_STORAGE_KEY);
    // 若為早期測試硬編碼之舊固定 IP，平滑遷移為 mDNS 預設網域名稱
    if (saved && (saved.includes('192.168.0.164') || saved.includes('10.154.175.134'))) {
      localStorage.setItem(CAMERA_URL_STORAGE_KEY, DEFAULT_CAMERA_URL);
      return DEFAULT_CAMERA_URL;
    }
    return saved || DEFAULT_CAMERA_URL;
  });
  
  // Layer 0: Chest Hardware Emergency Brake Telemetry
  const [chestTelemetry, setChestTelemetry] = useState<ChestTelemetry>(DEFAULT_CHEST_TELEMETRY);
  const [apiKeyInput, setApiKeyInput] = useState(() => getGeminiApiKey());
  const [showSettingsModal, setShowSettingsModal] = useState(false);
  const [chestStatus, setChestStatus] = useState<ChestStatus | null>(null);

  // Layer 1: Edge YOLO Detection State (預設暫停，避免自動持續分析)
  const [trackingEnabled, setTrackingEnabled] = useState(false);
  const [detections, setDetections] = useState<Detection[]>([]);
  const edgeDetectorRef = useRef<LocalYoloEdgeDetector | null>(null);
  const lastSpokenThreatRef = useRef<{ label: string; threatLevel: ThreatLevel; timestamp: number }>({
    label: '',
    threatLevel: 'safe',
    timestamp: 0,
  });
  const clearObstacleCounterRef = useRef<number>(0);

  // Layer 2: Cloud Gemini Description & Voice Q&A State
  const [describing, setDescribing] = useState(false);
  const [conversation, setConversation] = useState<ConversationItem[]>([]);
  const [textQuestion, setTextQuestion] = useState('');
  const [useChestSpeaker, setUseChestSpeaker] = useState(true);
  const [isListening, setIsListening] = useState(false);
  const recognitionRef = useRef<any>(null);

  // 實體按鍵與音訊計數器追蹤 (防重複觸發，-1 代表初次尚未同步)
  const lastTriggerCounterRef = useRef<number>(-1);
  const lastAudioCounterRef = useRef<number>(-1);
  const isHandlingVoiceRef = useRef<boolean>(false);
  const describingRef = useRef<boolean>(false);
  const analysisGenerationRef = useRef<number>(0);
  const inferenceInFlightRef = useRef<boolean>(false);
  // 語音播放冷卻期：防止喇叭聲音被麥克風收音觸發回饋循環
  const systemSpeakingRef = useRef<boolean>(false);
  const playbackCooldownUntilRef = useRef<number>(0);

  // ESP32 相機串流狀態
  const [streamError, setStreamError] = useState<boolean>(false);
  const [isReconnecting, setIsReconnecting] = useState<boolean>(false);

  // System States
  const [error, setError] = useState('');
  const [demoMode, setDemoMode] = useState(false);
  const [lastEvent, setLastEvent] = useState('系統就緒：三層邊緣穿戴式視覺輔助架構已啟動');
  const streamImgRef = useRef<HTMLImageElement | null>(null);
  const lastUrgentBrakeRef = useRef(false);
  const [isRealModel, setIsRealModel] = useState(false);
  const [lastInferenceMs, setLastInferenceMs] = useState(0);

  // 推論輪詢間隔 (ms)：預設 60ms ≈ 16 FPS，可根據裝置效能調整
  const INFERENCE_INTERVAL_MS = 60;

  const applyCameraUrl = (url: string) => {
    const normalized = url.trim().replace(/\/$/, '');
    setCameraUrl(normalized);
    localStorage.setItem(CAMERA_URL_STORAGE_KEY, normalized);
    setStreamError(false);
    setLastEvent(`已切換 ESP32 相機位址至：${normalized}`);
  };

  const reconnectStream = () => {
    setIsReconnecting(true);
    setStreamError(false);
    if (streamImgRef.current) {
      const base = cameraStreamUrl(cameraUrl);
      streamImgRef.current.src = `${base}${base.includes('?') ? '&' : '?'}t=${Date.now()}`;
    }
    setTimeout(() => setIsReconnecting(false), 800);
  };

  // 初始化第 1 層本地邊緣 YOLO 偵測器
  useEffect(() => {
    const detector = new LocalYoloEdgeDetector();
    detector.load().then(() => {
      edgeDetectorRef.current = detector;
      const isReal = detector.isRealModelRunning();
      setIsRealModel(isReal);
      setLastEvent(isReal ? '第 1 層：ONNX YOLOv8n 硬體加速模型載入就緒' : '第 1 層：手機本地 YOLO 動態感知就緒 (開發模擬模式)');
    });
  }, []);

  // 第 1 層：本地推論輪詢循環 (約 10~15 FPS，30~50ms 延遲)
  useEffect(() => {
    if (!trackingEnabled) {
      setDetections([]);
      return;
    }

    const interval = window.setInterval(async () => {
      // 推論可能超過一個輪詢週期；禁止並行推論堆積，否則會造成「停不下來」的分析與語音佇列。
      if (inferenceInFlightRef.current) return;
      const detector = edgeDetectorRef.current;
      if (!detector || !detector.isLoaded()) return;

      const img = streamImgRef.current;
      if (img && img.naturalWidth > 0) {
        try {
          inferenceInFlightRef.current = true;
          // 僅在 demoMode 開啟時才允許模擬假目標，避免真實相機無故捏造幽靈行人
          const results = await detector.detect(img, demoMode);
          setDetections(results);

          // 記錄推論耗時，供 UI 顯示效能資訊
          const inferMs = detector.getLastInferenceMs();
          if (inferMs > 0) setLastInferenceMs(inferMs);

          // 空間安全錐危險過濾 (僅針對中央安全錐之 warning / critical 目標)
          const criticalInCenter = results.find(
            (item) => item.zone === 'center' && (item.threatLevel === 'critical' || item.threatLevel === 'warning')
          );

          if (criticalInCenter) {
            clearObstacleCounterRef.current = 0;
            const now = Date.now();
            const last = lastSpokenThreatRef.current;

            const isNewTarget = last.label !== criticalInCenter.chineseLabel;
            const isEscalated = last.threatLevel === 'warning' && criticalInCenter.threatLevel === 'critical';
            // 同一物體持續存在時，延長至 25 秒才做低頻背景提醒，避免持續轟炸聽覺
            const isLongIntervalRemind = (now - last.timestamp > 25000);

            if (isNewTarget || isEscalated || isLongIntervalRemind) {
              const distanceText = criticalInCenter.threatLevel === 'critical' ? '緊急靠近中' : '前方約三公尺';
              const message = `注意，正前方有${criticalInCenter.chineseLabel}，${distanceText}`;
              
              if (useChestSpeaker && cameraUrl) {
                // 優先由胸前板喇叭真人語音播報提醒，並輔以嗶聲
                void triggerChestBeep(cameraUrl);
                speakWithCooldown(message, true, (msg) => setLastEvent(msg));
              } else {
                speak(message, criticalInCenter.threatLevel === 'critical' ? 'urgent' : 'normal');
              }

              lastSpokenThreatRef.current = {
                label: criticalInCenter.chineseLabel,
                threatLevel: criticalInCenter.threatLevel,
                timestamp: now,
              };
              setLastEvent(`動態告警：正前方偵測到${criticalInCenter.chineseLabel} (${criticalInCenter.estimatedDistanceM}m)`);
            }
          } else {
            // 連續約 1 秒以上未在中央安全錐發現重大威脅，才重置告警狀態
            clearObstacleCounterRef.current += 1;
            if (clearObstacleCounterRef.current > 12) {
              lastSpokenThreatRef.current = {
                label: '',
                threatLevel: 'safe',
                timestamp: 0,
              };
            }
          }
        } catch (e) {
          // 推論暫態忽略：若為 CORS 或 canvas 污染錯誤，提醒用戶檢查 ESP32 相機跨域設定
          const msg = String(e ?? '');
          if (msg.includes('security') || msg.includes('tainted') || msg.includes('cross-origin') || msg.includes('CORS')) {
            setStreamError(true);
            setLastEvent('⚠️ ESP32 相機串流跨域 (CORS) 受限，YOLO 無法讀取畫面像素。請確認 ESP32 已允許跨域存取，或改用 localhost 測試。');
          }
        } finally {
          inferenceInFlightRef.current = false;
        }
      }
    }, INFERENCE_INTERVAL_MS);

    return () => window.clearInterval(interval);
  }, [trackingEnabled, demoMode, cameraUrl, useChestSpeaker]);

  // 第 2 層：胸前雙向語音提問處理核心
  async function handleChestVoiceQandA() {
    if (isHandlingVoiceRef.current) return;
    // 冷卻期內禁止觸發新語音問答（防止喇叭聲音被收音觸發回饋循環）
    const nowMs = Date.now();
    if (nowMs < playbackCooldownUntilRef.current) {
      const remainingSec = Math.ceil((playbackCooldownUntilRef.current - nowMs) / 1000);
      setLastEvent(`⏸️ 系統語音冷卻中（剩餘 ${remainingSec} 秒），請稍後再提問`);
      return;
    }
    isHandlingVoiceRef.current = true;
    const generation = ++analysisGenerationRef.current;
    setDescribing(true);
    describingRef.current = true;
    setError('');
    setLastEvent('🎙️ 接收到胸前麥克風提問！正在抓取照片與音訊送往 Gemini…');

    try {
      // 1. 優先從目前胸前板畫面瞬間快照 (避免與 ESP32 MJPEG 串流連線搶佔卡死)，並同步抓取錄音檔
      const image = await captureCurrentStreamFrame(streamImgRef.current, cameraUrl);
      const audio = await fetchLatestAudio(cameraUrl);

      // 2. 呼叫 Gemini 多模態問答 (包含語音辨識 STT 轉寫與回答)
      const qaResult = await askGeminiWithImageAndAudio(image, audio);

      if (generation !== analysisGenerationRef.current) return;
      
      // 3. 語音朗讀回答 (優先推播至 ESP32 胸前板喇叭，失敗時本機發聲)
      speakWithCooldown(qaResult.answer, useChestSpeaker, (s) => setLastEvent(s));

      // 4. 更新對話與紀錄 (將使用者問的話清楚呈現於畫面上)
      const newEntry: ConversationItem = {
        id: String(Date.now()),
        type: 'voice',
        question: qaResult.userQuestion,
        answer: qaResult.answer,
        timestamp: Date.now(),
      };
      setConversation((prev) => [newEntry, ...prev.slice(0, 10)]);
      setLastEvent(`🎙️ 使用者問：「${qaResult.userQuestion}」 ➔ AI 回答：「${qaResult.answer}」`);
    } catch (err) {
      const errMsg = err instanceof Error ? err.message : '語音問答失敗';
      setError(errMsg);
      setLastEvent(`❌ 語音問答錯誤：${errMsg}`);
    } finally {
      if (generation === analysisGenerationRef.current) {
        setDescribing(false);
        describingRef.current = false;
        isHandlingVoiceRef.current = false;
      }
    }
  }

  // 免手按語音即時聆聽 (Web Speech API)
  function toggleListening() {
    if (isListening) {
      if (recognitionRef.current) {
        try { recognitionRef.current.stop(); } catch { /* empty */ }
        recognitionRef.current = null;
      }
      setIsListening(false);
      setLastEvent('已關閉免手按語音聆聽');
      return;
    }

    // 冷卻期內禁止開啟聆聽（防止喇叭聲音被收音觸發回饋循環）
    const nowMs = Date.now();
    if (nowMs < playbackCooldownUntilRef.current) {
      const remainingSec = Math.ceil((playbackCooldownUntilRef.current - nowMs) / 1000);
      setError(`系統語音播放中，請等待 ${remainingSec} 秒後再開啟聆聽`);
      return;
    }

    const SpeechRecognition = (window as unknown as { SpeechRecognition?: any; webkitSpeechRecognition?: any }).SpeechRecognition ||
                              (window as unknown as { SpeechRecognition?: any; webkitSpeechRecognition?: any }).webkitSpeechRecognition;

    if (!SpeechRecognition) {
      setError('您的瀏覽器不支援 Web Speech 語音辨識，請使用 Chrome / Edge 瀏覽器或點擊快捷按鈕提問。');
      return;
    }

    try {
      const recognition = new SpeechRecognition();
      recognition.lang = 'zh-TW';
      recognition.continuous = false;
      recognition.interimResults = false;

      recognition.onstart = () => {
        setIsListening(true);
        setLastEvent('🎙️ 正在聆聽中… 請直接開口說出問題（例如：「前面是什麼店家？」）');
      };

      recognition.onresult = (event: any) => {
        const transcript = event.results?.[0]?.[0]?.transcript;
        if (transcript && transcript.trim()) {
          // 辨識到一句話後立即關閉麥克風，絕不自動重複聆聽
          try { recognition.stop(); } catch {}
          setIsListening(false);
          setTextQuestion(transcript.trim());
          setLastEvent(`🎙️ 聽取到提問：「${transcript.trim()}」，正在分析畫面與回答…`);
          void describeEnvironment(transcript.trim());
        }
      };

      recognition.onerror = (event: any) => {
        setIsListening(false);
        try { recognition.stop(); } catch {}
      };

      recognition.onend = () => {
        setIsListening(false);
      };

      recognitionRef.current = recognition;
      recognition.start();
    } catch (err) {
      setIsListening(false);
      setError(err instanceof Error ? err.message : '啟動語音辨識失敗');
    }
  }

  // 第 2 層：手動文字或點擊情境描述
  async function describeEnvironment(customQuestion?: string) {
    if (describingRef.current || isHandlingVoiceRef.current) return;
    const generation = ++analysisGenerationRef.current;
    setError('');
    setDescribing(true);
    describingRef.current = true;
    try {
      const image = await captureCurrentStreamFrame(streamImgRef.current, cameraUrl);
      if (!image && !customQuestion) {
        throw new Error('胸前 ESP32 相機尚未連線，無法取得畫面；請確認下方相機位址或先使用文字提問。');
      }
      const qaResult = customQuestion
        ? await askGeminiWithImageAndAudio(image, null, customQuestion)
        : await askGeminiWithImageAndAudio(image);

      if (generation !== analysisGenerationRef.current) return;

      // 智慧語音輸出：優先推播給 ESP32 胸前板喇叭播放
      speakWithCooldown(qaResult.answer, useChestSpeaker, (s) => setLastEvent(s));

      const newEntry: ConversationItem = {
        id: String(Date.now()),
        type: customQuestion ? 'text' : 'scene',
        question: qaResult.userQuestion,
        answer: qaResult.answer,
        timestamp: Date.now(),
      };
      setConversation((prev) => [newEntry, ...prev.slice(0, 10)]);
      setLastEvent(customQuestion ? `🤖 問答完成：${qaResult.answer}` : '第 2 層認知：已完成單張環境深度描述');
      if (customQuestion) setTextQuestion('');
    } catch (descriptionError) {
      const msg = descriptionError instanceof Error ? descriptionError.message : '環境描述失敗';
      setError(msg);
      setLastEvent(`❌ 提問失敗：${msg}`);
    } finally {
      if (generation === analysisGenerationRef.current) {
        setDescribing(false);
        describingRef.current = false;
      }
    }
  }

  // 第 0 層 & 實體按鍵遙測輪詢 (/status)
  useEffect(() => {
    if (demoMode) return;
    if (!cameraUrl) return;

    let isMounted = true;
    let timerId: number | null = null;
    let failCount = 0;

    const pollStatus = async () => {
      try {
        const status: ChestStatus = await fetchChestStatus(cameraUrl);
        if (!isMounted) return;

        failCount = 0; // 重設失敗計數
        setChestStatus(status);

        const ultrasonicCm = status.ultrasonicCm ?? 400;
        const emergencyBrake = Boolean(status.emergencyBrake || ultrasonicCm < 80);

        setChestTelemetry({
          ultrasonicCm,
          emergencyBrake,
          connected: true,
          lastUpdated: Date.now(),
        });

        // 第 0 層急煞僅更新狀態與胸前板警示，不透過電腦喇叭持續轟炸播報
        if (emergencyBrake && !lastUrgentBrakeRef.current) {
          setLastEvent('第 0 層急煞：胸前硬體偵測距離小於 80cm！');
        }
        lastUrgentBrakeRef.current = emergencyBrake;

        // 偵測胸前板「按住提問錄音完畢放開」事件
        if (lastAudioCounterRef.current === -1 && status.audioCounter !== undefined) {
          lastAudioCounterRef.current = status.audioCounter;
        } else if (
          status.audioCounter !== undefined &&
          lastAudioCounterRef.current !== -1 &&
          status.audioCounter > lastAudioCounterRef.current
        ) {
          lastAudioCounterRef.current = status.audioCounter;
          lastTriggerCounterRef.current = status.triggerCounter;
          void handleChestVoiceQandA();
        }

        // 偵測胸前板「短按拍照快門」事件
        if (lastTriggerCounterRef.current === -1 && status.triggerCounter !== undefined) {
          lastTriggerCounterRef.current = status.triggerCounter;
        } else if (
          status.triggerCounter !== undefined &&
          lastTriggerCounterRef.current !== -1 &&
          status.triggerCounter > lastTriggerCounterRef.current
        ) {
          lastTriggerCounterRef.current = status.triggerCounter;
          void describeEnvironment();
        }
      } catch {
        if (!isMounted) return;
        failCount++;
        // 連續失敗 3 次以上（約 2.5 秒以上完全無連線）才判定為離線，防止 Wi-Fi 抖動誤報
        if (failCount >= 3) {
          setChestTelemetry((prev) => ({ ...prev, connected: false }));
        }
        // 連續失敗 4 次時，在背景自動觸發一次探測，看相機是否已切換至 mDNS 或熱點
        if (failCount === 4 && !isProbingRef.current) {
          void autoDiscoverCamera(false);
        }
      } finally {
        if (isMounted) {
          timerId = window.setTimeout(pollStatus, 800); // 間隔 800ms，給 ESP32 充足餘裕
        }
      }
    };

    pollStatus();

    return () => {
      isMounted = false;
      if (timerId !== null) window.clearTimeout(timerId);
    };
  }, [cameraUrl, demoMode]);

  const isProbingRef = useRef<boolean>(false);

  // 自動動態探測胸前相機 IP / mDNS 網址
  const autoDiscoverCamera = async (isManual = false): Promise<boolean> => {
    if (isProbingRef.current) return false;
    isProbingRef.current = true;
    if (isManual) {
      announceFeedback('正在自動搜尋胸前相機中…', 600);
    }

    const candidateUrls = [
      'http://visualassist.local', // 1. mDNS 區網網域名稱 (優先，免固定 IP)
      'http://192.168.4.1',        // 2. 戶外 SoftAP 熱點固定位址
      cameraUrl,                  // 3. 當前設定值
      localStorage.getItem(CAMERA_URL_STORAGE_KEY) || '',
    ].filter((u, i, arr) => u && arr.indexOf(u) === i);

    for (const candidate of candidateUrls) {
      const normalized = candidate.trim().replace(/\/$/, '');
      if (!normalized) continue;
      try {
        const controller = new AbortController();
        const tid = setTimeout(() => controller.abort(), 1200);
        const res = await fetch(`${normalized}/status`, {
          method: 'GET',
          cache: 'no-store',
          signal: controller.signal,
        });
        clearTimeout(tid);
        if (res.ok) {
          const data = await res.json();
          if (typeof data.triggerCounter === 'number') {
            applyCameraUrl(normalized);
            const typeStr = normalized.includes('visualassist.local')
              ? 'mDNS 區域網域'
              : normalized.includes('192.168.4.')
              ? '戶外熱點'
              : '區域 IP';
            announceFeedback(`已自動連線至胸前相機 (${typeStr})`, 750, [50, 50]);
            isProbingRef.current = false;
            return true;
          }
        }
      } catch {
        // 繼續嘗試下一個候選位址
      }
    }

    isProbingRef.current = false;
    if (isManual) {
      announceFeedback('未搜尋到胸前相機，請確認 ESP32 電源與連線', 350, [100]);
    }
    return false;
  };

  // 開機 1.5 秒後，若尚未連線成功則自動背景發起一次相機探測
  useEffect(() => {
    const timer = setTimeout(() => {
      if (!chestTelemetry.connected && !isProbingRef.current) {
        void autoDiscoverCamera(false);
      }
    }, 1500);
    return () => clearTimeout(timer);
  }, []);

  // 盲人專用：即時觸覺 (震動) 與聽覺 (音效 + 語音播報)
  const announceFeedback = (text: string, toneFreq = 600, haptic: number | number[] = 50) => {
    playTone(toneFreq, 70);
    if (typeof navigator !== 'undefined' && navigator.vibrate) {
      try {
        navigator.vibrate(haptic);
      } catch {
        // ignore
      }
    }
    speak(text, 'urgent');
    setLastEvent(text);
  };

  const handleStopAll = () => {
    setTrackingEnabled(false);
    setDescribing(false);
    analysisGenerationRef.current += 1;
    describingRef.current = false;
    isHandlingVoiceRef.current = false;
    inferenceInFlightRef.current = false;
    cancelSpeech();
    if (recognitionRef.current) {
      try {
        recognitionRef.current.stop();
      } catch {
        // ignore
      }
    }
    setIsListening(false);
    systemSpeakingRef.current = false;
    playbackCooldownUntilRef.current = 0;
    announceFeedback('已靜音，已停止所有分析與語音', 300, 100);
  };

  const handleReportStatus = () => {
    const connStr = chestTelemetry.connected ? '胸前相機已連線' : '胸前相機離線';
    const distStr = `前方距離約 ${Math.round(chestTelemetry.ultrasonicCm)} 公分`;
    const yoloStr = trackingEnabled ? '避障防撞功能運作中' : '避障防撞已暫停';
    const statusMsg = `系統狀態：${connStr}，${distStr}，${yoloStr}。`;
    announceFeedback(statusMsg, 750, [60, 60]);
  };

  // 統一語音播放包裝：播放時設置系統說話狀態，播放後啟動冷卻期防止回饋循環
  function speakWithCooldown(text: string, useChest: boolean, onStatus?: (msg: string) => void) {
    systemSpeakingRef.current = true;
    // 估計播放時間：中文每秒約 4-5 字，加上 2 秒緩衝
    const estimatedPlayMs = Math.max(3000, text.length * 250 + 2000);
    playbackCooldownUntilRef.current = Date.now() + estimatedPlayMs;
    void speakResponse(text, cameraUrl, useChest, (msg) => {
      onStatus?.(msg);
      // 播放結束後稍後清除系統說話狀態
      setTimeout(() => { systemSpeakingRef.current = false; }, 500);
    });
  }

  return (
    <main className="app-shell accessible-shell">
      {/* 頂部極簡無障礙工具列 */}
      <header className="accessible-topbar">
        <button
          type="button"
          className="topbar-btn setting-btn"
          aria-label="開啟系統設定"
          onClick={() => {
            const next = !showSettingsModal;
            setShowSettingsModal(next);
            announceFeedback(next ? '開啟系統設定' : '關閉設定', 650);
          }}
        >
          ⚙️ 設定
        </button>

        <button
          type="button"
          className={`connection-pill ${chestTelemetry.connected ? 'online' : 'offline'}`}
          aria-label={chestTelemetry.connected ? '胸前相機連線正常，點擊播報' : '胸前相機未連線，點擊播報'}
          onClick={() => {
            announceFeedback(
              chestTelemetry.connected
                ? '胸前相機連線正常，超音波與即時影像運作中'
                : '胸前相機離線，請檢查 ESP32 電源與 Wi-Fi 連線',
              600
            );
          }}
        >
          <span className="status-dot" />
          <span>{chestTelemetry.connected ? '相機已連線' : '相機離線'}</span>
        </button>

        <button
          type="button"
          className="topbar-btn stop-btn"
          aria-label="停止所有語音與分析"
          onClick={handleStopAll}
        >
          ⏹️ 停止
        </button>
      </header>

      {/* 系統設定彈窗 (收納所有次要與工程除錯按鈕) */}
      {showSettingsModal && (
        <section className="settings-modal-card" role="dialog" aria-modal="true" aria-label="系統設定與維護">
          <div className="settings-modal-header">
            <h3>⚙️ 系統設定與工程維護</h3>
            <button
              type="button"
              className="secondary btn-small"
              onClick={() => {
                setShowSettingsModal(false);
                announceFeedback('關閉設定', 500);
              }}
            >
              ✕ 關閉
            </button>
          </div>

          <div className="settings-section">
            <h4>🔑 Google Gemini API Key</h4>
            <div className="settings-row">
              <input
                type="password"
                placeholder="貼上 AI Studio API Key (AIzaSy...)"
                value={apiKeyInput}
                onChange={(e) => setApiKeyInput(e.target.value)}
              />
              <button
                type="button"
                className="primary"
                onClick={() => {
                  setGeminiApiKey(apiKeyInput);
                  announceFeedback(apiKeyInput ? 'API Key 已儲存' : '已清除 API Key', 700);
                }}
              >
                儲存
              </button>
              {apiKeyInput && (
                <button
                  type="button"
                  className="secondary danger-btn"
                  onClick={() => {
                    setApiKeyInput('');
                    setGeminiApiKey('');
                    announceFeedback('已清除 API Key', 400);
                  }}
                >
                  清除
                </button>
              )}
            </div>
          </div>

          <div className="settings-section">
            <h4>📷 胸前 ESP32 相機位址 (IP)</h4>
            <div className="settings-row">
              <input
                value={cameraUrl}
                onChange={(e) => setCameraUrl(e.target.value)}
                placeholder="http://visualassist.local"
              />
              <button
                type="button"
                className="primary"
                onClick={() => {
                  applyCameraUrl(cameraUrl);
                  announceFeedback('已套用相機位址', 600);
                }}
              >
                套用
              </button>
            </div>
            <div className="quick-ip-buttons">
              <button
                type="button"
                className="secondary btn-small"
                onClick={() => void autoDiscoverCamera(true)}
              >
                🔍 自動搜尋相機
              </button>
              <button
                type="button"
                className="secondary btn-small"
                onClick={() => applyCameraUrl('http://visualassist.local')}
              >
                🌐 mDNS (visualassist.local)
              </button>
              <button
                type="button"
                className="secondary btn-small"
                onClick={() => applyCameraUrl('http://192.168.4.1')}
              >
                📶 戶外熱點 (192.168.4.1)
              </button>
              <button
                type="button"
                className="secondary btn-small"
                onClick={reconnectStream}
              >
                🔄 重整串流
              </button>
            </div>
          </div>

          <div className="settings-section">
            <h4>🔊 語音播放輸出目標</h4>
            <div className="settings-row">
              <button
                type="button"
                className={`target-pill ${useChestSpeaker ? 'active' : ''}`}
                onClick={() => {
                  setUseChestSpeaker(true);
                  announceFeedback('已切換為胸前板喇叭播放', 650);
                }}
              >
                🔊 胸前板喇叭 (MAX98357)
              </button>
              <button
                type="button"
                className={`target-pill ${!useChestSpeaker ? 'active' : ''}`}
                onClick={() => {
                  setUseChestSpeaker(false);
                  announceFeedback('已切換為手機喇叭播放', 650);
                }}
              >
                📱 手機喇叭
              </button>
            </div>
          </div>

          <div className="settings-section">
            <h4>🛠️ 硬體檢測與展示模式</h4>
            <div className="quick-ip-buttons">
              <button
                type="button"
                className="secondary btn-small"
                onClick={async () => {
                  announceFeedback('正在發送嗶聲測試', 700);
                  const ok = await triggerChestBeep(cameraUrl);
                  announceFeedback(ok ? '胸前板嗶聲成功' : '胸前板喇叭無回應', ok ? 800 : 350);
                }}
              >
                🎵 測試嗶聲
              </button>
              <button
                type="button"
                className="secondary btn-small"
                onClick={() => {
                  speakWithCooldown('系統語音測試正常，我可以為您報路', useChestSpeaker, (m) => setLastEvent(m));
                }}
              >
                🗣️ 測試真人語音
              </button>
              <button
                type="button"
                className={`secondary btn-small ${demoMode ? 'selected' : ''}`}
                onClick={() => {
                  const next = !demoMode;
                  setDemoMode(next);
                  announceFeedback(next ? '已啟用展示模擬模式' : '已關閉展示模式', 600);
                }}
              >
                {demoMode ? '關閉展示模式' : '啟用展示模式'}
              </button>
            </div>
          </div>
        </section>
      )}

      {error && <section className="error-banner" role="alert">{error}</section>}

      {/* 鏡頭畫面 (保留給陪同者、弱視與視野確認) */}
      <section className="camera-view-container" aria-label="胸前即時視野畫面">
        <div
          className="stream-frame"
          tabIndex={0}
          role="img"
          aria-label={`胸前即時畫面，前方距離約 ${Math.round(chestTelemetry.ultrasonicCm)} 公分`}
          onClick={() => {
            const cm = Math.round(chestTelemetry.ultrasonicCm);
            const detStr = detections.length > 0
              ? `，偵測到${detections.map((d) => d.chineseLabel).join('、')}`
              : '，前方無重大障礙';
            announceFeedback(`前方畫面視野，超音波測距約 ${cm} 公分${detStr}`, 500);
          }}
          onKeyDown={(e) => {
            if (e.key === 'Enter' || e.key === ' ') {
              e.preventDefault();
              const cm = Math.round(chestTelemetry.ultrasonicCm);
              const detStr = detections.length > 0
                ? `，偵測到${detections.map((d) => d.chineseLabel).join('、')}`
                : '，前方無重大障礙';
              announceFeedback(`前方畫面視野，超音波測距約 ${cm} 公分${detStr}`, 500);
            }
          }}
        >
          {cameraUrl ? (
            <img
              ref={streamImgRef}
              className="camera-stream"
              src={cameraStreamUrl(cameraUrl)}
              alt="胸前即時視野畫面"
              crossOrigin="anonymous"
              onLoad={() => setStreamError(false)}
              onError={() => setStreamError(true)}
            />
          ) : (
            <div className="empty-frame">相機位址未設定</div>
          )}

          {/* 中央安全錐標線 */}
          <div className="safety-cone-overlay" />

          {/* YOLO 邊界框 */}
          {trackingEnabled &&
            detections.map((det) => {
              const [x, y, w, h] = det.bbox;
              return (
                <div
                  key={det.id}
                  className={`bbox-overlay ${det.threatLevel}`}
                  style={{
                    left: `${(x * 100).toFixed(1)}%`,
                    top: `${(y * 100).toFixed(1)}%`,
                    width: `${(w * 100).toFixed(1)}%`,
                    height: `${(h * 100).toFixed(1)}%`,
                  }}
                >
                  <span className="bbox-label">
                    {det.chineseLabel} {Math.round(det.confidence * 100)}% ({det.estimatedDistanceM}m)
                  </span>
                </div>
              );
            })}

          {streamError && (
            <div className="stream-error-overlay">
              <p>⚠️ 鏡頭串流未連線</p>
              <button
                type="button"
                className="primary btn-small"
                onClick={(e) => {
                  e.stopPropagation();
                  reconnectStream();
                  announceFeedback('正在重新連線相機串流', 600);
                }}
              >
                🔄 重新連線
              </button>
            </div>
          )}
        </div>
      </section>

      {/* 第 0 層：前方即時距離與急煞狀態條 (可點擊播報) */}
      <section
        className={`accessible-distance-bar ${chestTelemetry.emergencyBrake ? 'critical' : 'safe'}`}
        role="region"
        aria-live="polite"
        tabIndex={0}
        onClick={() => {
          const cm = Math.round(chestTelemetry.ultrasonicCm);
          const stateStr = chestTelemetry.emergencyBrake ? '急煞危險，距離過近' : '前方安全';
          announceFeedback(`前方障礙物距離約 ${cm} 公分，${stateStr}`, chestTelemetry.emergencyBrake ? 300 : 700, [60]);
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            const cm = Math.round(chestTelemetry.ultrasonicCm);
            const stateStr = chestTelemetry.emergencyBrake ? '急煞危險，距離過近' : '前方安全';
            announceFeedback(`前方障礙物距離約 ${cm} 公分，${stateStr}`, chestTelemetry.emergencyBrake ? 300 : 700, [60]);
          }
        }}
      >
        <div className="distance-info">
          <span className="distance-icon">
            {chestTelemetry.emergencyBrake ? '🚨' : '🟢'}
          </span>
          <span className="distance-text">
            {chestTelemetry.emergencyBrake
              ? `急煞警報！前方障礙小於 80cm！(${Math.round(chestTelemetry.ultrasonicCm)} cm)`
              : `前方安全 · 超音波距離：${Math.round(chestTelemetry.ultrasonicCm)} cm`}
          </span>
        </div>
        <span className="distance-tap-hint">點擊播報</span>
      </section>

      {/* 盲人巨型觸控按鈕區 (超大觸摸面積 + 語音回饋) */}
      <section className="accessible-buttons-panel" aria-label="盲人快捷操作區">
        {/* 巨型按鈕 1：拍照解讀眼前環境 */}
        <button
          type="button"
          className={`giant-btn giant-btn-primary ${describing ? 'danger' : ''}`}
          aria-label={describing ? '分析中，點擊強制取消' : '拍照解讀眼前景物'}
          onClick={() => {
            if (describing) {
              setDescribing(false);
              isHandlingVoiceRef.current = false;
              cancelSpeech();
              announceFeedback('已手動取消雲端分析', 400, [50]);
              return;
            }
            announceFeedback('正在拍照並分析眼前景物，請稍候…', 700, [70]);
            void describeEnvironment();
          }}
        >
          <span className="giant-btn-icon">{describing ? '⏹️' : '📷'}</span>
          <div className="giant-btn-content">
            <strong className="giant-btn-title">
              {describing ? '分析中… (點擊立即取消)' : '拍照解讀眼前景物'}
            </strong>
            <span className="giant-btn-desc">
              {describing ? '點擊立即中斷分析並安靜' : '短按拍照，AI 語音為您報路與環境描述'}
            </span>
          </div>
        </button>

        {/* 巨型按鈕 2：語音開口提問 */}
        <button
          type="button"
          className={`giant-btn giant-btn-voice ${isListening ? 'listening' : ''}`}
          aria-label={isListening ? '正在聆聽提問，點擊取消' : '語音提問，點擊後直接說話詢問'}
          disabled={describing}
          onClick={() => {
            if (isListening) {
              toggleListening();
              announceFeedback('已取消語音提問', 400, [40]);
            } else {
              announceFeedback('正在啟動語音辨識，請直接開口提問…', 650, [40, 40]);
              toggleListening();
            }
          }}
        >
          <span className="giant-btn-icon">{isListening ? '🔴' : '🎙️'}</span>
          <div className="giant-btn-content">
            <strong className="giant-btn-title">
              {isListening ? '正在聆聽提問中… (點擊取消)' : '語音提問 (開口詢問)'}
            </strong>
            <span className="giant-btn-desc">
              {isListening ? '請說出問題，說完後自動拍照分析回答' : '點擊開口說問題（例如「前面是什麼店？」）'}
            </span>
          </div>
        </button>

        {/* 底部雙列巨型開關 (避障防撞 + 停止語音) */}
        <div className="giant-btn-dual-row">
          {/* 巨型按鈕 3：避障防撞開關 (YOLO) */}
          <button
            type="button"
            className={`giant-btn giant-btn-toggle ${trackingEnabled ? 'active' : 'paused'}`}
            aria-label={`即時避障防撞功能目前${trackingEnabled ? '開啟中，點擊暫停' : '已暫停，點擊啟動'}`}
            onClick={() => {
              const nextState = !trackingEnabled;
              setTrackingEnabled(nextState);
              if (!nextState) {
                setDetections([]);
                cancelSpeech();
                announceFeedback('已暫停即時避障防撞', 400, [50]);
              } else {
                announceFeedback('已開啟即時避障防撞動態感知', 750, [50, 50]);
              }
            }}
          >
            <span className="giant-btn-icon">{trackingEnabled ? '🛡️' : '⏸️'}</span>
            <div className="giant-btn-content">
              <strong className="giant-btn-title">
                {trackingEnabled ? '避障：運作中' : '避障：已暫停'}
              </strong>
              <span className="giant-btn-desc">
                {trackingEnabled ? '點擊暫停避障' : '點擊開啟避障'}
              </span>
            </div>
          </button>

          {/* 巨型按鈕 4：停止語音與狀態回報 */}
          <button
            type="button"
            className="giant-btn giant-btn-stop"
            aria-label="停止語音，雙擊播報系統狀態"
            onClick={handleStopAll}
            onDoubleClick={handleReportStatus}
            onContextMenu={(e) => {
              e.preventDefault();
              handleReportStatus();
            }}
          >
            <span className="giant-btn-icon">🛑</span>
            <div className="giant-btn-content">
              <strong className="giant-btn-title">停止語音 / 狀態</strong>
              <span className="giant-btn-desc">短按靜音 · 雙擊報狀態</span>
            </div>
          </button>
        </div>
      </section>

      {/* 最新回答卡片 (供弱視或同伴閱讀，點擊可重新朗讀) */}
      {conversation.length > 0 && (
        <section
          className="accessible-qa-card"
          tabIndex={0}
          role="region"
          aria-label="AI 最新回覆，點擊重新朗讀"
          onClick={() => {
            announceFeedback(`重新朗讀回答：${conversation[0].answer}`, 550, [30]);
          }}
        >
          <div className="accessible-qa-header">
            <span className="qa-badge">🤖 AI 最新回答 (點擊重聽)</span>
            {conversation[0].question && (
              <span className="qa-user-q">問：{conversation[0].question}</span>
            )}
          </div>
          <p className="accessible-qa-text">{conversation[0].answer}</p>
        </section>
      )}

      {/* 底部即時狀態簡報 */}
      <footer className="accessible-footer" onClick={handleReportStatus} tabIndex={0}>
        <span className="footer-status-text">ℹ️ {lastEvent}</span>
      </footer>
    </main>
  );
}

export default App;
