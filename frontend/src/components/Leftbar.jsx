import React from 'react';
import './../styles/Leftbar.css';
import { LayoutDashboard, Users, LogOut, ShieldCheck, CalendarOff, FileChartColumn, Settings } from 'lucide-react';

const menuData = [
  { id: 'Dashboard', label: 'Dashboard', icon: <LayoutDashboard size={20} /> },
  { id: 'Employees', label: 'Employees', icon: <Users size={20} /> },
  { id: 'Permission', label: 'Permissions', icon: <ShieldCheck size={20} /> },
  { id: 'Leave', label: 'Leave', icon: <CalendarOff size={20} /> },
  { id: 'Report', label: 'Report', icon: <FileChartColumn size={20} /> },
];

export const Leftbar = ({ activeTab, onSelect }) => {
  return (
    <aside className="leftbar">
      <nav style={{ flex: 1 }}>
        {menuData.map(item => (
          <div
            key={item.id}
            className={`leftbar-item ${activeTab === item.id ? 'active' : ''}`}
            onClick={() => onSelect(item.id)}
          >
            {item.icon} {item.label}
          </div>
        ))}
      </nav>
      <div className="leftbar-item" onClick={() => onSelect(null)}>
        <Settings size={20} /> Setting
      </div>
      <div className="leftbar-item" onClick={() => onSelect(null)}>
        <LogOut size={20} /> Logout
      </div>
    </aside>
  );
};

export default Leftbar;