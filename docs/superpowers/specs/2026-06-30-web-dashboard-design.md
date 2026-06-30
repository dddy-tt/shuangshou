# Web Dashboard Design

- Date: 2026-06-30
- Repo: `dddy-tt/shuangshou`
- Scope: create a pure frontend mock dashboard under `web-dashboard/`

## Goal

Add a standalone Vite + React + TypeScript + Tailwind CSS demo app at the repository root to showcase the training dashboard flow for the glove project without modifying STM32 firmware files or Keil project files.

## Constraints

- Only create and edit files under `web-dashboard/` and this spec file.
- Do not modify `shuangshou/`, `Core/Src`, `Core/Inc`, or Keil project files.
- No backend, login system, API key, or real DeepSeek integration.
- First version must be pure frontend mock and runnable locally.
- Development server should run on port `3000`.
- `npm install` and `npm run build` must pass.

## Product Shape

The dashboard is a single-page app with two top tabs:

1. `训练看板`
2. `家电远控`

The default tab is `训练看板`.

## Training Dashboard

The main tab emphasizes the training flow in this order:

1. Current training task
2. Recognition result
3. AI-assisted feedback
4. Training statistics
5. Recent 10 records

### Interaction

- User selects a target gesture.
- Clicking `开始训练采集` starts a 3-second countdown.
- After countdown, the app generates a mock recognition result.
- Result displays correct or incorrect state.
- Stats update total count, correct count, streak, and recent 10 records.
- AI feedback is simulated with delayed frontend text only.

## Smart Home Tab

The secondary tab shows a local-only control mock for:

- light
- fan
- socket
- SOS

All buttons toggle client-side state only.

## Content Adjustments

The implementation must remove medicalized wording.

Required wording changes:

- `AI 康复医学指导` -> `AI 康复训练辅助反馈`
- `AI 医疗大模型点评` -> `AI 康复训练建议`
- `物联网控制图传` -> `物联网家电远控`

README and page copy must avoid phrases such as:

- `医疗诊断`
- `医学指导`
- `康复医学大模型`

## Technical Structure

The app should keep the requested file layout:

```text
web-dashboard/
├── package.json
├── tailwind.config.js
├── postcss.config.js
├── vite.config.ts
├── index.html
├── README.md
└── src/
    ├── main.tsx
    ├── index.css
    ├── App.tsx
    ├── types/
    │   └── index.ts
    ├── hooks/
    │   └── useWebSocket.ts
    ├── services/
    │   └── aiService.ts
    └── components/
        ├── StatusCard.tsx
        ├── TaskCard.tsx
        ├── ResultCard.tsx
        ├── FeedbackCard.tsx
        ├── StatsCard.tsx
        └── IotCard.tsx
```

## Implementation Notes

- Preserve the Gemini structure and intent, but fix broken strings, malformed JSX, missing README content, and any build-breaking issues.
- Remove the `lint` script if no ESLint config is provided.
- Keep types simple and local to the app.
- Use mock state and timers instead of external services.

## Verification

Acceptance requires:

- `npm install` succeeds
- `npm run build` succeeds
- `npm run dev` can serve on `http://localhost:3000`
- training interaction generates mock results
- correct and incorrect states render
- AI mock feedback renders
- training stats update
- recent 10 records display
- smart home buttons toggle state
- no STM32 firmware files are modified
