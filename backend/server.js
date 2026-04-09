const express = require('express');
const cors = require('cors');
const app = express();
const PORT = 5000;

// Allow Frontend (port 3000) call data from Backend (port 5000)
app.use(cors());
app.use(express.json());

// Data pattern (Virtual Database)
const employeesData = [
  { id: 1, name: 'Nguyễn Văn A', position: 'Developer', status: 'Active' },
  { id: 2, name: 'Trần Thị B', position: 'Designer', status: 'On Leave' },
  { id: 3, name: 'Lê Văn C', position: 'Manager', status: 'Active' },
];

const dashboardStats = {
  totalEmployees: 150,
  pendingLeave: 5,
  activeProjects: 12
};

// ------------------------------ APIs Endpoints ------------------------------

// Dashboard API
app.get('/api/dashboard', (req, res) => {
  res.json(dashboardStats);
});

// Employees API
app.get('/api/employees', (req, res) => {
  res.json(employeesData);
});

// Permissions API
app.get('/api/permissions', (req, res) => {
  res.json(['Admin', 'Editor', 'Viewer']);
});

// ----------------------------------------------------------------------------

// --- Listening request from frontend ---
app.listen(PORT, () => {
  console.log(`==> Starting API http://localhost:${PORT}`);
});