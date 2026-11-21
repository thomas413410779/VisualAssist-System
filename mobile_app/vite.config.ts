import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { VitePWA } from 'vite-plugin-pwa';

export default defineConfig({
  plugins: [
    react(),
    VitePWA({
      registerType: 'autoUpdate',
      includeAssets: ['favicon.ico', 'apple-touch-icon.png', 'models/yolov8n.onnx'],
      manifest: {
        name: 'Blind Assist - 視障智慧輔助系統',
        short_name: 'BlindAssist',
        description: '三層穿戴式邊緣視覺避障與認知前瞻輔助系統',
        theme_color: '#0b1220',
        background_color: '#070b14',
        display: 'standalone',
        orientation: 'portrait',
        icons: [
          {
            src: '/pwa-192x192.png',
            sizes: '192x192',
            type: 'image/png',
          },
          {
            src: '/pwa-512x512.png',
            sizes: '512x512',
            type: 'image/png',
          },
          {
            src: '/pwa-512x512.png',
            sizes: '512x512',
            type: 'image/png',
            purpose: 'any maskable',
          },
        ],
      },
      workbox: {
        // 放寬至 50MB，確保 12.2MB YOLO 模型與 28MB WASM 完整快取
        maximumFileSizeToCacheInBytes: 50 * 1024 * 1024,
        globPatterns: ['**/*.{js,css,html,ico,png,svg,wasm,onnx}'],
        runtimeCaching: [
          {
            // ESP32 相機串流、拍照與遙測端點：一律走網路直連，絕不快取
            urlPattern: /^(http|https):\/\/.*(:\d+)?\/(status|capture|audio|play|beep).*/i,
            handler: 'NetworkOnly',
          },
          {
            // Gemini 雲端 API：一律走網路直連
            urlPattern: /^https:\/\/generativelanguage\.googleapis\.com\/.*/i,
            handler: 'NetworkOnly',
          },
        ],
      },
    }),
  ],
  server: {
    host: true,
    port: 5173,
    proxy: {
      '/api/tts': {
        target: 'https://translate.google.com',
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api\/tts/, '/translate_tts'),
        headers: {
          'Referer': 'https://translate.google.com/',
          'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36'
        }
      }
    }
  },
});
