import { Routes, Route, Navigate } from 'react-router-dom';
import MainLayout from './layouts/MainLayout';
import DashboardPage from './pages/DashboardPage';
import EmployeesPage from './pages/EmployeesPage';
import LeavePage from './pages/LeavePage';
import ReportPage from './pages/ReportPage';
import ProfilePage from './pages/ProfilePage';
import AssetsPage from './pages/AssetsPage';
import SettingsPage from './pages/SettingsPage';
import LoginPage from './pages/LoginPage';
import RegisterPage from './pages/RegisterPage';

import ProtectedRoute from './components/ProtectedRoutes';

const AppRoutes = () => {
  return (
    <Routes>
      <Route element={<MainLayout />}>
        <Route path="/" element={<Navigate to="/login" replace />} />
        <Route path="*" element={<Navigate to="/login" replace />} />
        <Route path="/dashboard" element={
          <ProtectedRoute>
            <DashboardPage />
          </ProtectedRoute>
        } />
        <Route path="/employees" element={
          <ProtectedRoute>
            <EmployeesPage />
          </ProtectedRoute>
        } />
        <Route path="/leave" element={
          <ProtectedRoute>
            <LeavePage />
          </ProtectedRoute>
        } />
        <Route path="/report" element={
          <ProtectedRoute>
            <ReportPage />
          </ProtectedRoute>
        } />
        <Route path="/profile" element={
          <ProtectedRoute>
            <ProfilePage />
          </ProtectedRoute>
        } />
        <Route path="/assets" element={
          <ProtectedRoute>
            <AssetsPage />
          </ProtectedRoute>
        } />
        <Route path="/settings" element={<SettingsPage />} />
        <Route path="/login" element={<LoginPage />} />
        <Route path="/register" element={<RegisterPage />} />
      </Route>
    </Routes>
  );
};

export default AppRoutes;