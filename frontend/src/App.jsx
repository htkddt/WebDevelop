import React, { useState } from 'react';
import './styles/App.css';
import { Sidebar } from './layouts/Sidebar';
import { LoginForm } from './components/LoginForm';

function App() {
  // State quản lý việc tab nào đang được chọn
  const [activeTab, setActiveTab] = useState(null);

  // Hàm render nội dung linh hoạt
  const renderContent = () => {
    if (!activeTab) return <LoginForm />;

    return (
      <div className="view-content animate-fade-in">
        <h1 style={{ fontSize: '5rem', color: '#ccc' }}>{activeTab}</h1>
        <p>Đây là view dành cho {activeTab}</p>
      </div>
    );
  };

  return (
    <div className="layout-container">
      {/* Truyền state và hàm thay đổi state xuống Sidebar */}
      <Sidebar activeTab={activeTab} onSelect={setActiveTab} />

      <main className="main-view">
        {renderContent()}
      </main>
    </div>
  );
}

export default App;