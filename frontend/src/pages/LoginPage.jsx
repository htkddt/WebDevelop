import LoginView from '../views/LoginView';
import './../styles/Login.css';

const LoginPage = () => {
  // Proccessing API logic
  const handleLoginSubmit = (data) => {
    console.log("Login data:", data);
  };

  return (
    <div className="app-content">
      <div className="login-page-wrapper">
        <LoginView onSubmit={handleLoginSubmit} />
      </div>
    </div>
  );
};

export default LoginPage;