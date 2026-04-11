import React, { useState, useEffect } from 'react';
import PermissionsView from '../views/PermissionsView';

const PermissionsPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const MOCK_DATA = ['Null', 'Null', 'Null'];

  const [roles, setRoles] = useState([]);
  const [loading, setLoading] = useState(true);
  const [connected, setConnected] = useState(true);

  useEffect(() => {
    // Call API
    fetch('http://localhost:5000/api/permissions')
      .then(res => {
        if (!res.ok) throw new Error("Connection error Backend");
        return res.json();
      })
      .then(data => {
        setRoles(data);
        setLoading(false);
      })
      .catch((err) => {
        console.error("Connection error Backend:", err);
        setRoles(MOCK_DATA);
        setLoading(false);
        setConnected(false);
      });
  }, []);

  return (
    <div className="app-content">
      <PermissionsView roles={roles} loading={loading} connected={connected} />
    </div>
  );
};

export default PermissionsPage;