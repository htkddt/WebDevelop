import React, { useState } from 'react';
import './styles/App.css';
import MainLayout from './layouts/MainLayout';
import LoginView from './views/LoginView';
// import DashboardView from './views/DashboardView';
// import EmployeesView from './views/EmployeesView';

function App() {
  // State quản lý việc tab nào đang được chọn
  const [activeTab, setActiveTab] = useState(null);

  // Hàm render nội dung linh hoạt
  const renderView = () => {
    switch (activeTab) {
      case 'Dashboard':
        return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>{activeTab}</h1>
      case 'Employees':
        return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>{activeTab}</h1>
      case 'Permission':
        return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>{activeTab}</h1>
      case 'Leave':
        return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>{activeTab}</h1>
      case 'Report':
        return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>{activeTab}</h1>
      default:
        return <LoginView />;
    }
  };

  return (
    <MainLayout activeTab={activeTab} onSelect={setActiveTab}>
      {renderView()}
    </MainLayout>
  );
}

export default App;