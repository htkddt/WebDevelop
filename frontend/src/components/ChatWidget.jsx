import React, { useState } from 'react';
import { MessageCircle, X, Send } from 'lucide-react';
import './../styles/ChatWidget.css';

const ChatWidget = () => {
  const [isOpen, setIsOpen] = useState(false);

  return (
    <div className={`chat-widget-wrapper ${isOpen ? 'open' : ''}`}>
      {/* Nội dung cửa sổ Chat */}
      {isOpen && (
        <div className="chat-window">
          <div className="chat-header">
            <h4>Hỗ trợ trực tuyến</h4>
            <button onClick={() => setIsOpen(false)}><X size={18} /></button>
          </div>
          <div className="chat-body">
            <p className="chat-bot-msg">Xin chào! Chúng tôi có thể giúp gì cho bạn?</p>
          </div>
          <div className="chat-footer">
            <input type="text" placeholder="Nhập tin nhắn..." />
            <button className="chat-send-btn"><Send size={18} /></button>
          </div>
        </div>
      )}

      {/* Nút Chat nổi */}
      <button
        className="chat-fab"
        onClick={() => setIsOpen(!isOpen)}
        title="Chat với chúng tôi"
      >
        {isOpen ? <X size={28} /> : <MessageCircle size={28} />}
        {!isOpen && <span className="notification-dot"></span>}
      </button>
    </div>
  );
};

export default ChatWidget;