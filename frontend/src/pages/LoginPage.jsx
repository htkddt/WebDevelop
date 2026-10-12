import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import PageContent from '../components/PageContent';
import { authApi } from '../services/api';
import { authStorage } from '../services/authStorage';
import LoginView from '../views/LoginView';

const LoginPage = () => {
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const navigate = useNavigate();

  const handleLogin = async (credentials) => {
    setLoading(true);
    setError('');

    try {
      const data = await authApi.login(credentials);
      authStorage.setSession(data);
      navigate('/dashboard');
    } catch (requestError) {
      setError(requestError.message || 'Unable to sign in.');
    } finally {
      setLoading(false);
    }
  };

  return (
    <PageContent>
      <LoginView onLogin={handleLogin} loading={loading} errorMessage={error} />
    </PageContent>
  );
};

export default LoginPage;
