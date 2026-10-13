import React, { useState, useRef, useEffect } from 'react';
import { useNavigate } from 'react-router-dom';
import { Search, Bell, Globe, User, Settings, FileText, LogOut } from 'lucide-react';
import './../styles/Topbar.css';
import './../styles/Users.css';
import './../styles/Search.css';
import LogoImg from './../assets/Logo.png';

const Topbar = () => {
  const [showProfile, setShowProfile] = useState(false);
  const dropdownRef = useRef(null);
  const navigate = useNavigate();

  const handleLogout = () => {
    localStorage.removeItem('token');
    localStorage.removeItem('user');

    // Triggered to Leftbar for checking token and show if login is successful
    //window.dispatchEvent(new Event("storage"));

    navigate('/login');
    setShowProfile(false);
  };

  // Close pop-up when user clicks anywhere
  useEffect(() => {
    const handleClickOutside = (event) => {
      if (dropdownRef.current && !dropdownRef.current.contains(event.target)) {
        setShowProfile(false);
      }
    };
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

  return (
    <header className="topbar">
      <div className="topbar-left">
        <img src={LogoImg} alt="179FC" className="topbar-logo" />
        <span className="topbar-brand">179FC</span>
      </div>

      <div className="topbar-center">
        <div className="search-wrapper">
          <Search size={20} className="search-icon" />
          <input type="text" placeholder="Search something..." className="search-input" />
        </div>
      </div>

      <div className="topbar-right">
        <button className="topbar-icon-btn">
          <Bell size={20} />
          <span className="topbar-notification-badge"></span>
        </button>

        <div className="topbar-language-selector">
          <Globe size={20} />
          <select className="topbar-language-select">
            <option value="en">EN</option>
            <option value="vi">VI</option>
          </select>
        </div>

        <div className="user-profile-container" ref={dropdownRef}>
          <div className="user-profile-trigger" onClick={() => setShowProfile(!showProfile)}>
            <img src={LogoImg} alt="179FC" className="user-trigger-custom-logo" />
            <div className="user-avatar-wrapper">
              <User size={20} />
              <span className="user-profile-online-status"></span>
            </div>
          </div>
          {showProfile && (
            <div className="user-profile-popup">
              <div className="user-profile-popup-header">
                <div className="user-profile-popup-avatar">
                  <User size={30} />
                </div>
                <div className="user-info">
                  <h4>Huỳnh Tuấn Kiệt</h4>
                  <p>Software Engineer I / Ho Chi Minh</p>
                </div>
              </div>

              <div className="user-popup-divider" />

              <div className="popup-menu">
                <div className="user-popup-item" onClick={() => navigate('/profile')}>
                  <User size={18} /> <span>My Profile</span>
                </div>
                <div className="user-popup-item" onClick={() => navigate('/assets')}>
                  <FileText size={18} /> <span>My Assets</span>
                </div>
                <div className="user-popup-item" onClick={() => navigate('/settings')}>
                  <Settings size={18} /> <span>Settings</span>
                </div>

                <div className="user-popup-divider" />

                <div className="user-popup-item logout" onClick={handleLogout}>
                  <LogOut size={18} /> <span>Logout</span>
                </div>
              </div>
            </div>
          )}
        </div>
      </div>
    </header>
  );
};

export default Topbar;