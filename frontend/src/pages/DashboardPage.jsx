import React, { useState, useEffect } from 'react';
import DashboardView from '../views/DashboardView';

const DashboardPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const MOCK_DATA = [
    {
      "totalEmployees": -1,
      "pendingLeave": -1,
      "activeProjects": -1
    }
  ];

  const [stats, setStats] = useState({});
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    // Call API
    fetch('http://localhost:5000/api/dashboard')
      .then(res => {
        if (!res.ok) throw new Error("Backend die rồi");
        return res.json();
      })
      .then(data => {
        setStats(data);
        setLoading(false);
      })
      .catch(err => {
        console.error("Connection error Backend:", err);
        setStats(MOCK_DATA);
        setLoading(false);
      });
  }, []);

  return (
    <div className="app-content">
      <DashboardView stats={stats} loading={loading} />;
    </div>
  );
};

export default DashboardPage;