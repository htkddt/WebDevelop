import { Navigate } from 'react-router-dom';

const ProtectedRoute = ({ children }) => {
  const token = localStorage.getItem('token'); // Kiểm tra xem có vé chưa

  if (!token) {
    // Nếu không có vé, đá về trang Login ngay lập tức
    return <Navigate to="/login" replace />;
  }

  return children;
};

export default ProtectedRoute;