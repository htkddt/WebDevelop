import React from 'react';
import Leftbar from '../components/Leftbar';
import Topbar from '../components/Topbar';

export const MainLayout = ({ children, activeTab, onSelect }) => {
  return (
    <div className="app-wrapper">
      {/* Bên trên: Topbar cố định */}
        <Topbar activeTab={activeTab} onSelect={onSelect} />
        <div className="app-main">
          {/* Bên trái: Leftbar cố định */}
          <Leftbar activeTab={activeTab} onSelect={onSelect} />
          <main className="main-content">
            {/* Bên phải: Vùng trắng full màn hình còn lại */}
            {children}
          </main>
        </div>
    </div>
  );
};

export default MainLayout;