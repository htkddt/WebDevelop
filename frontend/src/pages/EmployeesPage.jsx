import React, { useState, useEffect } from 'react';
import EmployeesView from '../views/EmployeesView';

const EmployeesPage = () => {
  // Proccessing API logic
  // const handleLoginSubmit = (data) => {
  //   console.log("Login data:", data);
  // };

  const [employees, setEmployees] = useState([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    // Call API
    fetch('http://localhost:5000/api/employees')
      .then((res) => res.json())
      .then((data) => {
        setEmployees(data);
        setLoading(false);
      })
      .catch((err) => {
        console.error("Connection error Backend:", err);
        setLoading(false);
      });
  }, []);

  return (
    <div className="app-content">
      <EmployeesView data={employees} loading={loading} />
    </div>
  );
};

export default EmployeesPage;