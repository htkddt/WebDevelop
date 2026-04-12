import React, { useState, useEffect } from 'react';
import EmployeesView from '../views/EmployeesView';

const EmployeesPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const MOCK_DATA = [
    { id: -1, name: "Null", role: "Null", email: "Null" }
  ];

  const API_URL = window.location.hostname === "localhost"
    ? "http://localhost:5000/api/users/employees"
    : "https://webdevelop-gnyi.onrender.com/api/users/employees";

  const [employees, setEmployees] = useState([]);
  const [loading, setLoading] = useState(true);
  const [connected, setConnected] = useState(true);

  useEffect(() => {
    // Call API
    fetch(API_URL)
      .then((res) => {
        if (!res.ok) throw new Error("Connection error Backend");
        return res.json();
      })
      .then((data) => {
        setEmployees(data);
        setLoading(false);
      })
      .catch((err) => {
        console.error("Connection error Backend:", err);
        setEmployees(MOCK_DATA);
        setLoading(false);
        setConnected(false);
      });
  }, []);

  return (
    <div className="app-content">
      <EmployeesView data={employees} loading={loading} connected={connected} />
    </div>
  );
};

export default EmployeesPage;