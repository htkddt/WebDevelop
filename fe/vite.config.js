import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vitejs.dev/config/
export default defineConfig({
    plugins: [react()],
    server: {
        watch: {
            usePolling: true, // Ép Vite kiểm tra file thay đổi liên tục
        },
        // Nếu bạn muốn đổi port cho giống bản v2 cũ
        port: 3000,
    },
})
