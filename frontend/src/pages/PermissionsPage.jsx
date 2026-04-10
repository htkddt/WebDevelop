import React, { useState, useEffect } from 'react';
import PermissionsView from '../views/PermissionsView';

const PermissionsPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const MOCK_DATA = ['Admin', 'Editor', 'Viewer'];

  const [roles, setRoles] = useState([]);
  const [loading, setIsLoading] = useState(true);

  useEffect(() => {
    // Call API
    fetch('http://localhost:5000/api/permissions')
      .then(res => res.json())
      .then(data => {
        setRoles(data);
        setIsLoading(false);
      })
      .catch((err) => {
        console.error("Connection error Backend:", err);
        setRoles(MOCK_DATA);
        setLoading(false);
      });
  }, []);

  return (
    <div className="app-content">
      <PermissionsView roles={roles} loading={loading} />
    </div>
  );
};

export default PermissionsPage;