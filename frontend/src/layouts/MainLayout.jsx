import React from 'react';
import Leftbar from '../components/Leftbar';

export const MainLayout = ({ children, activeTab, onSelect }) => {
  return (
    <div className="app-container">
      {/* Bên trái: Leftbar cố định */}
      <Leftbar activeTab={activeTab} onSelect={onSelect} />

      {/* Bên phải: Vùng trắng full màn hình còn lại */}
      <main className="main-content">
        {children}
      </main>
    </div>
  );
};

export default MainLayout;