import React from 'react';
import { Search, Bell, Globe, User, LogIn } from 'lucide-react';
import './../styles/Topbar.css';
import LogoImg from './../assets/Logo.png';

const Topbar = () => {
  return (
    <header className="topbar">
      {/* PHẦN TRÁI: Logo */}
      <div className="topbar-left">
        <img src={LogoImg} alt="179FC Logo" className="topbar-logo" />
        <span className="topbar-brand">179FC</span>
      </div>

      {/* PHẦN GIỮA: Thanh tìm kiếm */}
      <div className="topbar-center">
        <div className="search-wrapper">
          <Search size={18} className="search-icon" />
          <input type="text" placeholder="Search something..." className="search-input" />
        </div>
      </div>

      {/* PHẦN PHẢI: Icons, Ngôn ngữ, Login */}
      <div className="topbar-right">
        <button className="icon-btn">
          <Bell size={20} />
          <span className="notification-badge"></span>
        </button>

        <div className="language-selector">
          <Globe size={18} />
          <select className="lang-select">
            <option value="en">EN</option>
            <option value="vi">VI</option>
          </select>
        </div>

        <div className="user-profile">
          <div className="user-info">
            <span className="user-name">TERRALOGIC</span>
            <div className="user-avatar-wrapper">
               <User size={20} />
               <span className="online-status"></span>
            </div>
          </div>
        </div>
      </div>
    </header>
  );
};

export default Topbar;