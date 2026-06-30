/** @type {import('tailwindcss').Config} */
export default {
  content: ["./index.html", "./src/**/*.{js,ts,jsx,tsx}"],
  theme: {
    extend: {
      colors: {
        ink: "#0f172a",
        panel: "#f8fafc",
        signal: "#f59e0b",
        accent: "#0ea5e9",
        success: "#16a34a",
        danger: "#dc2626"
      },
      boxShadow: {
        panel: "0 24px 60px rgba(15, 23, 42, 0.12)"
      },
      animation: {
        float: "float 6s ease-in-out infinite"
      },
      keyframes: {
        float: {
          "0%, 100%": { transform: "translateY(0px)" },
          "50%": { transform: "translateY(-8px)" }
        }
      }
    }
  },
  plugins: []
}
