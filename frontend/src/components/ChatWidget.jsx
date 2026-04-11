import React, { useState, useEffect, useRef } from 'react';
import { MessageCircle, X, Send } from 'lucide-react';
import './../styles/ChatWidget.css';

const ChatWidget = () => {
  const [isOpen, setIsOpen] = useState(false);
  const [isTyping, setIsTyping] = useState(false);
  const [message, setMessage] = useState('');
  const [chatLog, setChatLog] = useState([
    { id: 'welcome', text: 'Sao đấy ní, cần tui giúp gì à?', sender: 'bot' }
  ]);

  const textareaRef = useRef(null);
  const chatBodyRef = useRef(null);

  const API_URL = window.location.hostname === "localhost"
    ? "http://localhost:5000/api/chat"
    : "https://webdevelop-gnyi.onrender.com/api/chat";

  // Hàm tự động điều chỉnh chiều cao
  const handleTextareaChange = (e) => {
    const textarea = e.target;
    setMessage(textarea.value);

    // Reset height về auto để tính toán lại chính xác
    textarea.style.height = 'auto';
    // Gán chiều cao mới dựa trên scrollHeight (tối đa do CSS quy định)
    textarea.style.height = `${textarea.scrollHeight}px`;
  };

  useEffect(() => {
    if (chatBodyRef.current) {
      chatBodyRef.current.scrollTop = chatBodyRef.current.scrollHeight;
    }
  }, [chatLog, isOpen]);

  const handleSendMessage = async (e) => {
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
    setIsTyping(true);
    if (textareaRef.current) {
      textareaRef.current.style.height = 'auto'; // Reset chiều cao về ban đầu
    }

    try {
      const response = await fetch(API_URL, {
        method: 'POST',
        headers: { 'type': 'json' },
        body: JSON.stringify({ contents: message }),
      });
      if (!response.ok) throw new Error("Connection error Backend");
      const data = await response.json();
      const botReply = {
        id: Date.now() + 1,
        text: data.header + data.reply,
        sender: 'bot'
      };
      setChatLog((prev) => [...prev, botReply]);

    } catch (error) {
      console.error("Connection error Backend:", error);
      // Set timeout response after 1 second
      setTimeout(() => {
        const botReply = {
          id: Date.now() + 1,
          text: 'Lêu lêu ní bị anh Klose dụ rồi, tui chưa có data ní ơi >..<',
          sender: 'bot'
        };
        setChatLog((prev) => [...prev, botReply]);
      }, 1000);
    } finally {
      setIsTyping(false);
    }
  };

  return (
    <div className="chat-widget-wrapper">
      <div className={`chat-window ${isOpen ? 'active' : 'inactive'}`}>
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
          {isTyping && (
            <div className="chat-message-row bot chat-typing-indicator">
              <span></span>
              <span></span>
              <span></span>
            </div>
          )}
        </div>

        <div className="chat-footer">
          <form className="chat-input-area" onSubmit={handleSendMessage}>
            <textarea
              ref={textareaRef}
              type="text"
              placeholder="Ask Klose"
              value={message}
              rows="1"
              onChange={handleTextareaChange}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && !e.shiftKey) {
                  e.preventDefault();
                  handleSendMessage();
                }
              }}
            />
            <button type="submit" className="chat-send-btn">
              <Send size={18} />
            </button>
          </form>
        </div>
      </div>

      <button className="chat-fab" onClick={() => setIsOpen(!isOpen)}>
        {isOpen ? <X size={28} /> : <MessageCircle size={28} />}
        {!isOpen && <span className="chat-notification-dot"></span>}
      </button>
    </div>
  );
};

export default ChatWidget;