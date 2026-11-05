export const GEMINI_API_KEY_STORAGE_KEY = 'visual-assist-gemini-key';

export function getGeminiApiKey(): string {
  if (typeof window !== 'undefined') {
    const userKey = localStorage.getItem(GEMINI_API_KEY_STORAGE_KEY);
    if (userKey && userKey.trim()) return userKey.trim();
  }
  return (import.meta.env.VITE_GEMINI_API_KEY as string | undefined) || '';
}

export function setGeminiApiKey(key: string): void {
  if (typeof window !== 'undefined') {
    if (!key.trim()) {
      localStorage.removeItem(GEMINI_API_KEY_STORAGE_KEY);
    } else {
      localStorage.setItem(GEMINI_API_KEY_STORAGE_KEY, key.trim());
    }
  }
}

const GEMINI_MODEL = (import.meta.env.VITE_GEMINI_MODEL as string | undefined) || 'gemini-flash-latest';

export interface GeminiQaResult {
  userQuestion: string;
  answer: string;
}

const DEFAULT_PROMPT = [
  '你是視障者的環境描述助理。',
  '請嚴格依照下列格式輸出兩行（繁體中文）：',
  '【使用者提問】：環境全景認知',
  '【助理回答】：依序描述正前方最重要的障礙物、可通行方向與地標（控制在 45 字以內）',
].join('\n');

const VOICE_QUESTION_PROMPT = [
  '你是視障者的即時語音問答助理。',
  '隨附內容包含：使用者透過胸前麥克風說話提問的音訊，以及相機現場照片。',
  '請嚴格依照下列兩行格式輸出，方便系統將提問轉寫並呈現於畫面上：',
  '【使用者提問】：將音訊中使用者所說的話完整精確轉寫為繁體中文（若聽不清楚或無人聲則寫「語音提問」）',
  '【助理回答】：結合照片針對使用者問題給出精確、親切、口語的繁體中文回答（60字以內）',
].join('\n');

function arrayBufferToBase64(buffer: ArrayBuffer): string {
  let binary = '';
  const bytes = new Uint8Array(buffer);
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + chunkSize));
  }
  return btoa(binary);
}

function parseGeminiResponse(rawText: string, fallbackQuestion?: string): GeminiQaResult {
  const qMatch = rawText.match(/【使用者提問】[：:]\s*(.*?)(?=\n|【助理回答】|$)/s);
  const aMatch = rawText.match(/【助理回答】[：:]\s*(.*)/s);

  if (qMatch && aMatch) {
    return {
      userQuestion: qMatch[1].trim() || (fallbackQuestion ?? '語音提問'),
      answer: aMatch[1].trim(),
    };
  }

  const lines = rawText.split('\n').map(l => l.trim()).filter(Boolean);
  if (lines.length >= 2 && (lines[0].includes('提問') || lines[0].includes('問題'))) {
    return {
      userQuestion: lines[0].replace(/【.*?】|^.*?[:：]/g, '').trim() || (fallbackQuestion ?? '語音提問'),
      answer: lines.slice(1).join(' ').replace(/【.*?】|^.*?[:：]/g, '').trim(),
    };
  }

  return {
    userQuestion: fallbackQuestion ?? '語音提問',
    answer: rawText.replace(/【.*?】/g, '').trim(),
  };
}

/**
 * 多模態 Gemini 認知問答
 * 支援「純文字提問」、「純照片描述」或「照片 + 使用者語音提問」同步發送
 */
export async function askGeminiWithImageAndAudio(
  imageBlob?: Blob | null,
  audioBlob?: Blob | null,
  textQuestion?: string
): Promise<GeminiQaResult> {
  const apiKey = getGeminiApiKey();
  if (!apiKey || apiKey === 'replace_with_your_key' || apiKey === 'your_gemini_api_key_here') {
    throw new Error('尚未設定 Gemini API Key；請在網頁右上角「設定 API Key」輸入您的 Google AI Studio API Key。');
  }

  const parts: Array<{ text?: string; inline_data?: { mime_type: string; data: string } }> = [];

  // 1. 設定提示詞與提問
  if (audioBlob) {
    parts.push({ text: VOICE_QUESTION_PROMPT });
  } else if (textQuestion) {
    const textPrompt = [
      '你是視障者的即時問答助理。請嚴格依照格式輸出兩行（繁體中文）：',
      `【使用者提問】：${textQuestion}`,
      '【助理回答】：針對問題給予口語、簡明且親切的回答（60字以內）',
    ].join('\n');
    parts.push({ text: textPrompt });
  } else {
    parts.push({ text: DEFAULT_PROMPT });
  }

  // 2. 加入照片 (若有提供)
  if (imageBlob && imageBlob.size > 0) {
    const imageBase64 = arrayBufferToBase64(await imageBlob.arrayBuffer());
    parts.push({
      inline_data: {
        mime_type: imageBlob.type || 'image/jpeg',
        data: imageBase64,
      },
    });
  }

  // 3. 若有語音提問音訊，加入音訊資料
  if (audioBlob && audioBlob.size > 0) {
    const audioBase64 = arrayBufferToBase64(await audioBlob.arrayBuffer());
    parts.push({
      inline_data: {
        mime_type: audioBlob.type || 'audio/wav',
        data: audioBase64,
      },
    });
  }

  const candidateModels = [GEMINI_MODEL, 'gemini-flash-lite-latest', 'gemini-3.8-flash'];
  const uniqueModels = Array.from(new Set(candidateModels));
  let lastError: Error | null = null;

  for (const model of uniqueModels) {
    const endpoint = `https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`;
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 12000);

    try {
      const response = await fetch(endpoint, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'X-goog-api-key': apiKey,
        },
        signal: controller.signal,
        body: JSON.stringify({
          contents: [{
            role: 'user',
            parts,
          }],
          generationConfig: {
            temperature: 0.2,
            maxOutputTokens: 256,
          },
        }),
      });
      clearTimeout(timeoutId);

      if (!response.ok) {
        const errorText = await response.text();
        console.warn(`[Gemini] 模型 ${model} 回傳 HTTP ${response.status}: ${errorText.slice(0, 100)}，嘗試備用模型...`);
        lastError = new Error(`HTTP ${response.status}: ${errorText.slice(0, 100)}`);
        continue;
      }

      const payload = (await response.json()) as {
        candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>;
      };
      const text = payload.candidates?.[0]?.content?.parts?.map((part) => part.text ?? '').join('').trim();
      if (text) {
        return parseGeminiResponse(text, textQuestion);
      }
    } catch (err) {
      clearTimeout(timeoutId);
      console.warn(`[Gemini] 模型 ${model} 請求失敗:`, err);
      lastError = err instanceof Error ? err : new Error(String(err));
    }
  }

  throw new Error(`Gemini 回應失敗：${lastError?.message || '所有模型皆無回應'}`);
}

/**
 * 向下相容原有函式呼叫 (僅回傳回答文字)
 */
export async function describeImageWithGemini(imageBlob: Blob): Promise<string> {
  const result = await askGeminiWithImageAndAudio(imageBlob);
  return result.answer;
}
