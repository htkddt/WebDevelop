import { Routes, Route, Navigate } from 'react-router-dom';
import MainLayout from './layouts/MainLayout';
import DashboardPage from './pages/DashboardPage';
import EmployeesPage from './pages/EmployeesPage';
import LeavePage from './pages/LeavePage';
import PermissionsPage from './pages/PermissionsPage';
import ReportPage from './pages/ReportPage';
import ProfilePage from './pages/ProfilePage';
import AssetsPage from './pages/AssetsPage';
import SettingsPage from './pages/SettingsPage';
import LoginPage from './pages/LoginPage';

const AppRoutes = () => {
  return (
    <Routes>
      <Route element={<MainLayout />}>
        <Route path="/" element={<Navigate to="/login" replace />} />
        <Route path="/dashboard" element={<DashboardPage />} />
        <Route path="/employees" element={<EmployeesPage />} />
        <Route path="/permissions" element={<PermissionsPage />} />
        <Route path="/leave" element={<LeavePage />} />
        <Route path="/report" element={<ReportPage />} />
        <Route path="/profile" element={<ProfilePage />} />
        <Route path="/assets" element={<AssetsPage />} />
        <Route path="/settings" element={<SettingsPage />} />
        <Route path="/login" element={<LoginPage />} />
      </Route>
    </Routes>
  );
};

export default AppRoutes;