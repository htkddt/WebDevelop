import { Navigate, Outlet } from 'react-router-dom';
import { authStorage } from '../services/authStorage';

const ProtectedRoute = ({ children }) => {
  const token = authStorage.getToken();

  if (!token) {
    // Nếu không có vé, đá về trang Login ngay lập tức
    return <Navigate to="/login" replace />;
  }

  return children || <Outlet />;
};

export default ProtectedRoute;
