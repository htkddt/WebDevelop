import React, { useState } from 'react';
import './../styles/Leftbar.css';
import { LayoutDashboard, Users, LogOut, ShieldCheck, CalendarOff, FileChartColumn, Settings, Menu, X } from 'lucide-react';

const menuData = [
  { id: 'Dashboard', label: 'Dashboard', icon: <LayoutDashboard size={20} /> },
  { id: 'Employees', label: 'Employees', icon: <Users size={20} /> },
  { id: 'Permission', label: 'Permissions', icon: <ShieldCheck size={20} /> },
  { id: 'Leave', label: 'Leave', icon: <CalendarOff size={20} /> },
  { id: 'Report', label: 'Report', icon: <FileChartColumn size={20} /> },
];

export const Leftbar = ({ activeTab, onSelect }) => {
  const [isExpanded, setIsExpanded] = useState(false);

  const toggleSidebar = () => {
    setIsExpanded(!isExpanded);
  };

  return (
    <aside className={`leftbar ${isExpanded ? 'expanded' : 'collapsed'}`}>
      <div className="leftbar-toggle-btn" onClick={toggleSidebar}>
        {isExpanded ? <X size={24} /> : <Menu size={24} />}
      </div>
      <nav style={{ flex: 1 }}>
        {menuData.map(item => (
          <div
            key={item.id}
            className={`leftbar-item ${activeTab === item.id ? 'active' : ''}`}
            onClick={() => onSelect(item.id)}
          >
            {item.icon}
            {isExpanded && <span className="leftbar-label">{item.label}</span>}
          </div>
        ))}
      </nav>
      <div className="leftbar-item" onClick={() => onSelect(null)}>
        <Settings size={20} />
        {isExpanded && <span className="leftbar-label">Setting</span>}
      </div>
      <div className="leftbar-item" onClick={() => onSelect(null)}>
        <LogOut size={20} />
        {isExpanded && <span className="leftbar-label">Logout</span>}
      </div>
    </aside>
  );
};

export default Leftbar;