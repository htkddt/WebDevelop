import { Search, Bell, Globe, User } from 'lucide-react';
import './../styles/Topbar.css';
import './../styles/Users.css';
import './../styles/Search.css';
import LogoImg from './../assets/Logo.png';

const Topbar = () => {
  return (
    <header className="topbar">
      <div className="topbar-left">
        <img src={LogoImg} alt="179FC Logo" className="topbar-logo" />
        <span className="topbar-brand">179FC</span>
      </div>

      <div className="topbar-center">
        <div className="search-wrapper">
          <Search size={20} className="search-icon" />
          <input type="text" placeholder="Search something..." className="search-input" />
        </div>
      </div>

      <div className="topbar-right">
        <button className="icon-btn">
          <Bell size={20} />
          <span className="notification-badge"></span>
        </button>

        <div className="language-selector">
          <Globe size={20} />
          <select className="language-select">
            <option value="en">EN</option>
            <option value="vi">VI</option>
          </select>
        </div>

        <div className="user-profile">
          <div className="user-info">
            <span className="user-name">Login</span>
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