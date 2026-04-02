import React from 'react';
import { LayoutDashboard, Users, CreditCard, LogOut } from 'lucide-react';

const menuData = [
  { id: 'Dashboard', label: 'Dashboard', icon: <LayoutDashboard size={20} /> },
  { id: 'Employees', label: 'Employees', icon: <Users size={20} /> },
  { id: 'Payroll', label: 'Payroll', icon: <CreditCard size={20} /> },
];

export const Sidebar = ({ activeTab, onSelect }) => {
  return (
    <aside className="sidebar">
      <div className="logo" style={{ fontSize: '24px', fontWeight: 'bold', marginBottom: '40px' }}>179FC</div>
      <nav style={{ flex: 1 }}>
        {menuData.map(item => (
          <div
            key={item.id}
            className={`menu-item ${activeTab === item.id ? 'active' : ''}`}
            onClick={() => onSelect(item.id)}
          >
            {item.icon} {item.label}
          </div>
        ))}
      </nav>
      <div className="menu-item" onClick={() => onSelect(null)}>
        <LogOut size={20} /> Logout
      </div>
    </aside>
  );
};