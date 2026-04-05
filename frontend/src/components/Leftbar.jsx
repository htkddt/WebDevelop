import React, { useState } from 'react';
import { NavLink } from 'react-router-dom';
import { LayoutDashboard, Users, LogOut, ShieldCheck, CalendarOff, FileChartColumn, Settings, Menu, X } from 'lucide-react';
import './../styles/Leftbar.css';

const menuData = [,
  { id: 'Dashboard', label: 'Dashboard', icon: <LayoutDashboard size={20} />, path: '/dashboard' },
  { id: 'Employees', label: 'Employees', icon: <Users size={20} />, path: '/employees' },
  { id: 'Permission', label: 'Permissions', icon: <ShieldCheck size={20} />, path: '/permissions' },
  { id: 'Leave', label: 'Leave', icon: <CalendarOff size={20} />, path: '/leave' },
  { id: 'Report', label: 'Report', icon: <FileChartColumn size={20} />, path: '/report' },
];

export const Leftbar = () => {
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
          <NavLink
            key={item.id}
            to={item.path}
            className={({ isActive }) => `leftbar-item ${isActive ? 'active' : ''}`}
          >
            {item.icon}
            {isExpanded && <span className="leftbar-label">{item.label}</span>}
          </NavLink>
        ))}
      </nav>
      <NavLink
        to="/settings"
        className={({ isActive }) => `leftbar-item ${isActive ? 'active' : ''}`}
      >
        <Settings size={20} />
        {isExpanded && <span className="leftbar-label">Setting</span>}
      </NavLink>
      <NavLink
        to="/login"
        className={({ isActive }) => `leftbar-item ${isActive ? 'active' : ''}`}
      >
        <LogOut size={20} />
        {isExpanded && <span className="leftbar-label">Logout</span>}
      </NavLink>
    </aside>
  );
};

export default Leftbar;