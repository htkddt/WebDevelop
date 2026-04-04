import React from 'react';
import Leftbar from '../components/Leftbar';
import Topbar from '../components/Topbar';
import './../styles/Frames.css';

export const MainLayout = ({ children, activeTab, onSelect }) => {
  return (
    <div className="layout-container">
      <Topbar activeTab={activeTab} onSelect={onSelect} />
      <div className="layout-body">
        <Leftbar activeTab={activeTab} onSelect={onSelect} />
        <main className="layout-content">
          {children}
        </main>
      </div>
    </div>
  );
};

export default MainLayout;