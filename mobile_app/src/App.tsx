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
import { cancelSpeech, speak, speakResponse } from './services/ttsService';
import {
  LocalYoloEdgeDetector,
  type Detection,
  type SpatialZone,
  type ThreatLevel,
} from './services/edgeDetector';
import {
  DEFAULT_CHEST_TELEMETRY,
  type ChestTelemetry,
  type GeminiDescription,
} from './types';
import './styles.css';

const CAMERA_URL_STORAGE_KEY = 'visual-assist-camera-url';

interface ConversationItem {
  id: string;
  type: 'voice' | 'text' | 'scene';
  question?: string;
  answer: string;
  timestamp: number;
}

function App() {
  const [cameraUrl, setCameraUrl] = useState(() => {
    const targetIp = 'http://10.154.175.134';
    const saved = localStorage.getItem(CAMERA_URL_STORAGE_KEY);
    if (!saved || saved.includes('192.168.0.164') || saved.includes('192.168.4.') || saved.includes('172.18.71.')) {
      localStorage.setItem(CAMERA_URL_STORAGE_KEY, targetIp);
      return targetIp;
    }
    return saved || targetIp;
  });
  
  // Layer 0: Chest Hardware Emergency Brake Telemetry
  const [chestTelemetry, setChestTelemetry] = useState<ChestTelemetry>(DEFAULT_CHEST_TELEMETRY);
  const [apiKeyInput, setApiKeyInput] = useState(() => getGeminiApiKey());
  const [showKeyModal, setShowKeyModal] = useState(false);
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
  const [, setDescription] = useState<GeminiDescription | null>(null);
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
      setDescription({ text: qaResult.answer, generatedAt: Date.now() });
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

      const nextDescription = { text: qaResult.answer, generatedAt: Date.now() };
      setDescription(nextDescription);

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

  function saveCameraUrl() {
    const normalized = cameraUrl.trim().replace(/\/$/, '');
    setCameraUrl(normalized);
    localStorage.setItem(CAMERA_URL_STORAGE_KEY, normalized);
    setLastEvent(`已儲存胸前相機位址：${normalized}`);
  }

  function stopAllSpeech() {
    cancelSpeech();
    systemSpeakingRef.current = false;
    playbackCooldownUntilRef.current = 0;
    setLastEvent('已停止所有語音播報');
  }

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
    <main className="app-shell">
      <header className="topbar">
        <div>
          <p className="eyebrow">VISUAL ASSIST · WEARABLE SYSTEM</p>
          <h1>穿戴式視覺輔助系統</h1>
          <p className="subtitle">第0層胸前急煞 + 第1層手機本地YOLO動態追蹤 + 第2層按需雲端認知</p>
        </div>
        <div className="status-pill-group">
          <button
            type="button"
            className="secondary btn-small"
            style={{
              background: '#1e293b',
              color: apiKeyInput ? '#4ade80' : '#f59e0b',
              border: apiKeyInput ? '1px solid #16a34a' : '1px solid #d97706',
              fontWeight: 'bold',
              cursor: 'pointer',
              padding: '6px 12px',
              borderRadius: '20px',
            }}
            onClick={() => setShowKeyModal((prev) => !prev)}
          >
            🔑 {apiKeyInput ? 'API Key 已配置' : '⚠️ 設定 API Key'}
          </button>
          <button
            type="button"
            className="secondary btn-small"
            style={{
              background: '#991b1b',
              color: '#ffffff',
              border: '1px solid #dc2626',
              fontWeight: 'bold',
              cursor: 'pointer',
              padding: '6px 12px',
              borderRadius: '20px',
            }}
            onClick={() => {
              setTrackingEnabled(false);
              setDescribing(false);
              analysisGenerationRef.current += 1;
              describingRef.current = false;
              isHandlingVoiceRef.current = false;
              inferenceInFlightRef.current = false;
              cancelSpeech();
              if (recognitionRef.current) {
                try { recognitionRef.current.stop(); } catch {}
              }
              setIsListening(false);
              setLastEvent('⏹️ 已立即停止所有 YOLO 追蹤、雲端分析與語音！');
            }}
          >
            ⏹️ 一鍵停止所有分析與語音
          </button>
          <div className={`connection-pill ${chestTelemetry.connected ? 'online' : 'offline'}`}>
            <span className="status-dot" /> {chestTelemetry.connected ? '胸前相機已連線' : '胸前相機離線'}
          </div>
        </div>
      </header>

      {showKeyModal && (
        <section className="card" style={{ marginBottom: '18px', border: '1px solid #38bdf8', background: '#0c1b30' }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '10px' }}>
            <h3 style={{ margin: 0, color: '#38bdf8' }}>🔑 Google Gemini API Key 安全設定</h3>
            <button
              type="button"
              className="secondary btn-small"
              onClick={() => setShowKeyModal(false)}
            >
              ✕ 關閉
            </button>
          </div>
          <p style={{ fontSize: '13px', color: '#94a3b8', margin: '0 0 12px' }}>
            安全防護：您的 API Key 將僅保存在本機瀏覽器的 <code>localStorage</code> 中，絕不上傳至伺服器或被打包至公開檔案中。
          </p>
          <div style={{ display: 'flex', gap: '10px' }}>
            <input
              type="password"
              placeholder="請貼上您的 Google AI Studio API Key (AIzaSy... 或 AQ....)"
              value={apiKeyInput}
              onChange={(e) => setApiKeyInput(e.target.value)}
              style={{ flex: 1 }}
            />
            <button
              type="button"
              className="primary"
              onClick={() => {
                setGeminiApiKey(apiKeyInput);
                setShowKeyModal(false);
                setLastEvent(apiKeyInput ? '✅ Gemini API Key 已成功儲存至本機！' : 'ℹ️ 已清除本機 API Key');
              }}
            >
              儲存
            </button>
            {apiKeyInput && (
              <button
                type="button"
                className="secondary"
                style={{ color: '#ef4444', borderColor: '#ef4444' }}
                onClick={() => {
                  setApiKeyInput('');
                  setGeminiApiKey('');
                  setLastEvent('ℹ️ 已清除本機 Gemini API Key');
                }}
              >
                清除
              </button>
            )}
          </div>
        </section>
      )}

      {error && <section className="error-banner" role="alert">{error}</section>}

      {/* 第 0 層：胸前保命急煞 (硬體反射神經) */}
      <section className={`danger-panel ${chestTelemetry.emergencyBrake ? 'critical' : 'safe'}`} aria-live="assertive">
        <div>
          <div className="layer-badge">第 0 層 · 胸前反射急煞 (&lt; 30ms)</div>
          <h2>{chestTelemetry.emergencyBrake ? '急煞警報！前方緊急障礙！' : '前方安全，未達急煞門檻'}</h2>
          <p>
            {chestTelemetry.emergencyBrake
              ? '胸前超音波測得小於 80cm 近迫障礙物，硬體蜂鳴器／震動已直接觸發急煞！'
              : '胸前超音波持續監控中，前方距離安全，系統保持正常通行。'}
          </p>
        </div>
        <div className="danger-number">
          <strong>{Math.round(chestTelemetry.ultrasonicCm)}</strong>
          <span>cm</span>
        </div>
      </section>

      <div className="main-grid">
        {/* 第 1 層：手機本地 YOLO 動態視覺感知卡片 */}
        <section className="card camera-card">
          <div className="card-heading">
            <div>
              <div className="layer-badge layer-1">第 1 層 · 手機邊緣動態 YOLO (15~30 FPS)</div>
              <h2>即時視野與動態追蹤</h2>
            </div>
            <div className="badge-group">
              <span className={`tag ${trackingEnabled ? 'active-tag' : ''}`}>
                {trackingEnabled ? 'YOLO 追蹤中' : '已暫停追蹤'}
              </span>
              <span className={`tag ${isRealModel ? 'active-tag' : ''}`}>
                {isRealModel ? '⚡ ONNX WebGL' : '🛠️ 模擬備援'}
              </span>
              <span className="tag">MJPEG</span>
              {trackingEnabled && lastInferenceMs > 0 && (
                <span className="tag" style={{ fontSize: '11px' }}>
                  ⏱️ {lastInferenceMs.toFixed(0)}ms
                </span>
              )}
            </div>
          </div>

          <div className="stream-container">
            {/* 安全錐指示標尺 */}
            <div className="spatial-guide-bar">
              <span className="zone-indicator left">左側</span>
              <span className="zone-indicator center active">中央安全錐</span>
              <span className="zone-indicator right">右側</span>
            </div>

            <div className="stream-frame">
              {cameraUrl ? (
                <img
                  ref={streamImgRef}
                  className="camera-stream"
                  src={cameraStreamUrl(cameraUrl)}
                  alt="胸前 ESP32 即時畫面"
                  crossOrigin="anonymous"
                  onLoad={() => setStreamError(false)}
                  onError={() => setStreamError(true)}
                />
              ) : (
                <div className="empty-frame">請先輸入 ESP32 相機位址</div>
              )}

              {streamError && (
                <div className="camera-error-hint">
                  <h4>⚠️ 尚未接收到 ESP32 相機串流 ({cameraUrl})</h4>
                  <p>請確認 ESP32 的連線狀態：</p>
                  <ul>
                    <li><b>情況 A：ESP32 剛開機或尚未連上 Wi-Fi</b><br/>請將電腦 Wi-Fi 連線至 ESP32 熱點 <b>BlindAssist-Camera</b>（密碼: <b>blindassist</b>），並點擊下方 <b>192.168.4.1</b>。</li>
                    <li><b>情況 B：ESP32 已連線至您的 Wi-Fi (00000000)</b><br/>請確認 ESP32 分配到的實際 IP（可從手機熱點連線裝置列表查看），並在下方直接填入連線。</li>
                  </ul>
                  <div className="quick-ip-buttons">
                    <button className="primary btn-small" onClick={() => applyCameraUrl('http://172.18.71.134')}>
                      🎯 立即連線 (172.18.71.134)
                    </button>
                    <button className="secondary btn-small" onClick={() => applyCameraUrl('http://192.168.4.1')}>
                      📡 熱點 AP (192.168.4.1)
                    </button>
                    <button className="secondary btn-small" onClick={reconnectStream}>
                      🔄 {isReconnecting ? '連線中…' : '重新整理影像串流'}
                    </button>
                  </div>
                  <div style={{ display: 'flex', gap: '8px', marginTop: '14px', justifyContent: 'center' }}>
                    <input
                      style={{ padding: '6px 10px', fontSize: '13px', width: '240px', borderRadius: '6px', background: '#0b1624', color: '#fff', border: '1px solid #32567a' }}
                      value={cameraUrl}
                      onChange={(e) => setCameraUrl(e.target.value)}
                      placeholder="輸入 ESP32 IP，例如 http://192.168.43.50"
                    />
                    <button className="primary btn-small" onClick={() => applyCameraUrl(cameraUrl)}>
                      儲存並連線
                    </button>
                  </div>
                </div>
              )}

              {/* 疊加中央安全錐視覺標線 */}
              <div className="safety-cone-overlay" />

              {/* 即時物件邊界框繪製 */}
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
            </div>
          </div>

          {/* 偵測物件清單 */}
          <div className="detection-list">
            <strong>目前視野動態物件：</strong>
            {detections.length === 0 ? (
              <span className="no-detection">無重大近距障礙</span>
            ) : (
              detections.map((det) => (
                <span key={det.id} className={`detection-pill ${det.threatLevel}`}>
                  {det.chineseLabel} · {det.zone === 'center' ? '正前方' : det.zone === 'left' ? '左側' : '右側'} ({det.estimatedDistanceM}m)
                </span>
              ))
            )}
          </div>

          <div className="button-row">
            <button
              className={trackingEnabled ? 'secondary' : 'primary'}
              onClick={() => {
                const nextState = !trackingEnabled;
                setTrackingEnabled(nextState);
                if (!nextState) {
                  setDetections([]);
                  cancelSpeech();
                  setLastEvent('⏹️ 已暫停 YOLO 即時追蹤與警報語音');
                } else {
                  setLastEvent('▶️ 已啟動 YOLO 即時動態感知追蹤');
                }
              }}
            >
              {trackingEnabled ? '⏹️ 暫停 YOLO 追蹤' : '▶️ 啟動 YOLO 追蹤'}
            </button>
            <button
              className={`secondary ${demoMode ? 'selected' : ''}`}
              onClick={() => setDemoMode((value) => !value)}
            >
              {demoMode ? '關閉展示模式' : '啟用展示模式'}
            </button>
          </div>

          <div className="form-row">
            <label htmlFor="camera-url">相機位址</label>
            <input
              id="camera-url"
              value={cameraUrl}
              onChange={(e) => setCameraUrl(e.target.value)}
              placeholder="http://192.168.4.1"
            />
            <button className="secondary" onClick={saveCameraUrl}>儲存</button>
          </div>
        </section>

        {/* 第 2 層：雲端多模態 AI 認知問答卡片 (雙向語音問答大腦) */}
        <section className="card cloud-card">
          <div className="card-heading">
            <div>
              <div className="layer-badge layer-2">第 2 層 · 雲端認知大腦 (按需多模態問答)</div>
              <h2>全景情境與雙向語音提問</h2>
            </div>
            <div className="badge-group">
              <span className={`tag ${chestStatus?.isRecording ? 'critical' : ''}`}>
                {chestStatus?.isRecording ? '🔴 胸前錄音中…' : chestStatus?.micReady ? '🎙️ PDM 麥克風就緒' : '麥克風未偵測'}
              </span>
              <span className="tag">
                {chestStatus?.speakerReady ? '🔊 喇叭擴大機' : 'Gemini 2.5'}
              </span>
            </div>
          </div>

          <p className="cloud-explain">
            本層專為讀取<strong>店家招牌、紅綠燈秒數、環境全景敘事與尋物</strong>設計。
          </p>

          <div className="voice-tip-box" style={{ borderLeft: '4px solid #10b981', background: 'rgba(16, 185, 129, 0.1)' }}>
            <span className="voice-tip-icon">🎯</span>
            <div className="voice-tip-text">
              <strong>純按鍵觸發模式已生效：</strong>
              系統平常保持完全靜態待命（絕對不會自動循環分析）。<strong>只有在您按住板子背後的 <code>BOOT (GPIO 0)</code> 鍵提問、放開後，AI 才會啟動分析一次並直接由胸前板喇叭回答！</strong>
            </div>
          </div>

          {/* 🔊 語音輸出目標與免手按語音開關 */}
          <div className="voice-mode-toolbar">
            <div className="speaker-target-row">
              <span className="toolbar-label">🔊 語音播放目標：</span>
              <button
                type="button"
                className={`target-pill ${useChestSpeaker ? 'active' : ''}`}
                onClick={() => setUseChestSpeaker(true)}
              >
                🔊 胸前板喇叭 (MAX98357)
              </button>
              <button
                type="button"
                className={`target-pill ${!useChestSpeaker ? 'active' : ''}`}
                onClick={() => setUseChestSpeaker(false)}
              >
                📱 手機/電腦喇叭
              </button>
              <button
                type="button"
                className="target-pill"
                style={{ background: '#2563eb', color: '#ffffff', border: '1px solid #3b82f6', fontWeight: 'bold' }}
                onClick={async () => {
                  setLastEvent('🔊 正在發送嗶聲測試至胸前板喇叭…');
                  const ok = await triggerChestBeep(cameraUrl);
                  setLastEvent(ok ? '🔊 [測試成功] 胸前板喇叭已發出嗶聲提示！' : '❌ 胸前板喇叭無回應，請確認相機連線。');
                }}
              >
                🎵 測試嗶聲
              </button>
              <button
                type="button"
                className="target-pill"
                style={{ background: '#059669', color: '#ffffff', border: '1px solid #10b981', fontWeight: 'bold' }}
                onClick={async () => {
                  setLastEvent('🗣️ 正在合成語音並推播至胸前喇叭…');
                  speakWithCooldown('視覺輔助系統已就緒，我可以為您報路', true, (msg) => setLastEvent(msg));
                }}
              >
                🗣️ 測試真人語音說話
              </button>
            </div>

            <button
              type="button"
              className={`mic-live-btn ${isListening ? 'listening' : ''}`}
              onClick={toggleListening}
              disabled={describing}
            >
              {isListening ? '🔴 正在聆聽您的問題… (說完自動拍照提問)' : '🎙️ 開啟免手按語音提問 (開口直接說)'}
            </button>
          </div>

          {/* 常用語音快捷提問按鈕 */}
          <div className="quick-question-row">
            <button
              className="chip-btn"
              disabled={describing}
              onClick={() => void describeEnvironment('這家店招牌寫什麼？販賣什麼？')}
            >
              🏷️ 店家招牌？
            </button>
            <button
              className="chip-btn"
              disabled={describing}
              onClick={() => void describeEnvironment('前面路口紅燈或綠燈還有幾秒？')}
            >
              🚦 紅綠燈秒數？
            </button>
            <button
              className="chip-btn"
              disabled={describing}
              onClick={() => void describeEnvironment('前方地面是否有階梯、水坑或施工障礙？')}
            >
              🚧 路況障礙？
            </button>
          </div>

          {/* 手動輸入文字提問 */}
          <form
            className="question-input-row"
            onSubmit={(e) => {
              e.preventDefault();
              if (textQuestion.trim()) {
                void describeEnvironment(textQuestion.trim());
              }
            }}
          >
            <input
              type="text"
              value={textQuestion}
              onChange={(e) => setTextQuestion(e.target.value)}
              placeholder="輸入問題或對胸前長按提問…"
              disabled={describing}
            />
            <button type="submit" className="primary" disabled={describing || !textQuestion.trim()}>
              {describing ? '分析中…' : '提問'}
            </button>
          </form>

          <button
            className={`wide trigger-btn ${describing ? 'danger' : 'secondary'}`}
            onClick={() => {
              if (describing) {
                setDescribing(false);
                isHandlingVoiceRef.current = false;
                cancelSpeech();
                setLastEvent('⏹️ 已手動取消雲端分析');
                return;
              }
              void describeEnvironment();
            }}
            disabled={!cameraUrl && !describing}
            style={describing ? { background: '#dc2626', color: '#ffffff', borderColor: '#b91c1c', cursor: 'pointer' } : {}}
          >
            {describing ? '⏹️ 雲端分析中… (點擊立即強制取消)' : '📷 單拍快門：解讀全景情境'}
          </button>

          {/* 最新提問與回答醒目焦點卡片 */}
          {conversation.length > 0 && (
            <div className="current-qa-card">
              <div className="qa-bubble user">
                <div className="qa-bubble-header">
                  <span className="qa-role-badge user-badge">
                    {conversation[0].type === 'voice' ? '🎙️ 您剛才問的話' : '⌨️ 您的提問'}
                  </span>
                </div>
                <p className="qa-text user-text">「{conversation[0].question}」</p>
              </div>

              <div className="qa-bubble ai">
                <div className="qa-bubble-header">
                  <span className="qa-role-badge ai-badge">🤖 Gemini 2.5 回答</span>
                </div>
                <p className="qa-text ai-text">{conversation[0].answer}</p>
              </div>
            </div>
          )}

          {/* 歷史問答對話歷程列表 */}
          {conversation.length > 1 && (
            <div className="conversation-panel">
              <strong>📜 歷史問答紀錄：</strong>
              <div className="conversation-list">
                {conversation.slice(1).map((item) => (
                  <div key={item.id} className="conversation-item">
                    {item.question && (
                      <div className="conv-question">
                        <span className="conv-tag">{item.type === 'voice' ? '🎙️ 語音' : '⌨️ 文字'}</span>
                        <span>{item.question}</span>
                      </div>
                    )}
                    <div className="conv-answer">
                      <span className="conv-tag ai">🤖 Gemini</span>
                      <p>{item.answer}</p>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          )}
        </section>
      </div>

      {/* 底部狀態與控制列 */}
      <section className="card protocol-card">
        <div>
          <p className="panel-kicker">系統即時日誌</p>
          <p className="last-event-text">{lastEvent}</p>
        </div>
        <button className="secondary" onClick={stopAllSpeech}>停止全部語音</button>
      </section>

      <footer className="footer-note">
        穿戴式視覺輔助系統原型 · 三層邊緣防護架構
      </footer>
    </main>
  );
}

export default App;
