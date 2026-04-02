import React from 'react';
import LogoImg from './../assets/logo.png';
import './../styles/Sidebar.css';
import { LayoutDashboard, Users, LogOut, ShieldCheck, CalendarOff, FileChartColumn, Settings } from 'lucide-react';

const menuData = [
  { id: 'Dashboard', label: 'Dashboard', icon: <LayoutDashboard size={20} /> },
  { id: 'Employees', label: 'Employees', icon: <Users size={20} /> },
  { id: 'Permission', label: 'Permissions', icon: <ShieldCheck size={20} /> },
  { id: 'Leave', label: 'Leave', icon: <CalendarOff size={20} /> },
  { id: 'Report', label: 'Report', icon: <FileChartColumn size={20} /> },
];

export const Sidebar = ({ activeTab, onSelect }) => {
  return (
    <aside className="sidebar">
      <div className="sidebar-header">
        <img src={LogoImg} alt="Logo" className="sidebar-logo-img" />
        <span className="sidebar-brand-name">179FC</span>
      </div>
      <nav style={{ flex: 1 }}>
        {menuData.map(item => (
          <div
            key={item.id}
            className={`sidebar-item ${activeTab === item.id ? 'active' : ''}`}
            onClick={() => onSelect(item.id)}
          >
            {item.icon} {item.label}
          </div>
        ))}
      </nav>
      <div className="sidebar-item" onClick={() => onSelect(null)}>
        <Settings size={20} /> Setting
      </div>
      <div className="sidebar-item" onClick={() => onSelect(null)}>
        <LogOut size={20} /> Logout
      </div>
    </aside>
  );
};

export default Sidebar;