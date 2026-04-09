import React, { useState, useEffect } from 'react';
import DashboardView from '../views/DashboardView';

const DashboardPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const [stats, setStats] = useState({});
  const [loading, setIsLoading] = useState(true);

  useEffect(() => {
    // Call API
    fetch('http://localhost:5000/api/dashboard')
      .then(res => res.json())
      .then(data => {
        setStats(data);
        setIsLoading(false);
      })
      .catch(err => console.error(err));
  }, []);

  return (
    <div className="app-content">
      <DashboardView stats={stats} loading={loading} />;
    </div>
  );
};

export default DashboardPage;