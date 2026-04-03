import React from 'react';
import Leftbar from '../components/Leftbar';
import Topbar from '../components/Topbar';
import './../styles/MainLayout.css';

export const MainLayout = ({ children, activeTab, onSelect }) => {
  return (
    <div className="layout-container">
      {/* Bên trên: Topbar cố định */}
      <Topbar activeTab={activeTab} onSelect={onSelect} />
      <div className="layout-body">
        {/* Bên trái: Leftbar cố định */}
        <Leftbar activeTab={activeTab} onSelect={onSelect} />
        <main className="layout-content">
          {/* Bên phải: Vùng trắng full màn hình còn lại */}
          {children}
        </main>
      </div>
    </div>
  );
};

export default MainLayout;