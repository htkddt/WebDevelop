import React, { useState, useEffect, useRef } from 'react';
import { MessageCircle, X, Send } from 'lucide-react';
import './../styles/ChatWidget.css';

const ChatWidget = () => {
  const [isOpen, setIsOpen] = useState(false);
  const [message, setMessage] = useState('');
  const [chatLog, setChatLog] = useState([
    { id: 'welcome', text: 'Sao đấy ní, cần tui giúp gì à?', sender: 'bot' }
  ]);
  const chatBodyRef = useRef(null);
  useEffect(() => {
    if (chatBodyRef.current) {
      chatBodyRef.current.scrollTop = chatBodyRef.current.scrollHeight;
    }
  }, [chatLog, isOpen]);

  const handleSendMessage = (e) => {
    if (e) e.preventDefault();
    if (message.trim() === '') return;

    const userMsg = {
      id: Date.now(),
      text: message,
      sender: 'user',
      time: new Date().toLocaleTimeString()
    };

    setChatLog((prev) => [...prev, userMsg]);
    setMessage('');

    // Set timeout response after 1 second
    setTimeout(() => {
      const botReply = {
        id: Date.now() + 1,
        text: 'Lêu lêu ní anh Klose bị dụ rồi, tui có dữ liệu đâu mà trả lời >..<',
        sender: 'bot'
      };
      setChatLog((prev) => [...prev, botReply]);
    }, 1000);
  };

  return (
    <div className={`chat-widget-wrapper ${isOpen ? 'open' : ''}`}>
      {isOpen && (
        <div className="chat-window">
          <div className="chat-header">
            <h4>Chat with me</h4>
          </div>

          <div className="chat-body" ref={chatBodyRef}>
            {chatLog.map((msg) => (
              <div key={msg.id} className={`chat-message-row ${msg.sender}`}>
                <div className="bubble">
                  {msg.text}
                </div>
              </div>
            ))}
          </div>

          <div className="chat-footer">
            <form className="chat-input-area" onSubmit={handleSendMessage}>
              <input
                type="text"
                placeholder="Ask Klose"
                value={message}
                onChange={(e) => setMessage(e.target.value)}
              />
              <button type="submit" className="chat-send-btn">
                <Send size={18} />
              </button>
            </form>
          </div>
        </div>
      )}

      <button className="chat-fab" onClick={() => setIsOpen(!isOpen)}>
        {isOpen ? <X size={28} /> : <MessageCircle size={28} />}
        {!isOpen && <span className="chat-notification-dot"></span>}
      </button>
    </div>
  );
};

export default ChatWidget;