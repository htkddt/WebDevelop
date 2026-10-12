import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import PageContent from '../components/PageContent';
import { authApi } from '../services/api';
import RegisterView from '../views/RegisterView';

const RegisterPage = () => {
  const [loading, setLoading] = useState(false);
  const navigate = useNavigate();

  const handleRegister = async (userData) => {
    setLoading(true);

    try {
      await authApi.register(userData);
      navigate('/login');
    } catch (error) {
      console.error('Backend connection error:', error);
    } finally {
      setLoading(false);
    }
  };

  return (
    <PageContent>
      <RegisterView onRegister={handleRegister} loading={loading} />
    </PageContent>
  );
};

export default RegisterPage;
