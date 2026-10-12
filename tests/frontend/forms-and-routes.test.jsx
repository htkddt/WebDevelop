import React from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import LoginView from '../../frontend/src/views/LoginView';
import RegisterView from '../../frontend/src/views/RegisterView';
import ProtectedRoute from '../../frontend/src/components/ProtectedRoutes';
import { authStorage } from '../../frontend/src/services/authStorage';

describe('authentication UI', () => {
  it('submits the complete company email and toggles password visibility', () => {
    const onLogin = jest.fn();
    render(<MemoryRouter><LoginView onLogin={onLogin} loading={false} errorMessage="" /></MemoryRouter>);

    fireEvent.change(screen.getByLabelText('Email'), { target: { value: 'kiet' } });
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'secret' } });
    fireEvent.click(screen.getByRole('button', { name: 'Show password' }));
    expect(screen.getByLabelText('Password')).toHaveAttribute('type', 'text');

    fireEvent.click(screen.getByRole('button', { name: 'Login now' }));
    expect(onLogin).toHaveBeenCalledWith({ email: 'kiet@klose.dev', password: 'secret' });
  });

  it('shows the supplied login error and disables submit while loading', () => {
    render(<MemoryRouter><LoginView onLogin={jest.fn()} loading errorMessage="Invalid credentials" /></MemoryRouter>);

    expect(screen.getByText('Invalid credentials. Please try again!')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Checking ...' })).toBeDisabled();
  });

  it('rejects a short registration password before calling the API', () => {
    const onRegister = jest.fn();
    jest.spyOn(window, 'alert').mockImplementation(() => {});
    render(<MemoryRouter><RegisterView onRegister={onRegister} loading={false} /></MemoryRouter>);

    fireEvent.change(screen.getByLabelText('Full Name'), { target: { value: 'Kiet' } });
    fireEvent.change(screen.getByLabelText('Email'), { target: { value: 'kiet' } });
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: '12345' } });
    fireEvent.change(screen.getByLabelText('Department'), { target: { value: 'Engineering' } });
    fireEvent.click(screen.getByRole('button', { name: 'Sign Up' }));

    expect(window.alert).toHaveBeenCalled();
    expect(onRegister).not.toHaveBeenCalled();
  });

  it('submits default role, permissions and status with a valid registration', () => {
    const onRegister = jest.fn();
    render(<MemoryRouter><RegisterView onRegister={onRegister} loading={false} /></MemoryRouter>);

    fireEvent.change(screen.getByLabelText('Full Name'), { target: { value: 'Kiet' } });
    fireEvent.change(screen.getByLabelText('Email'), { target: { value: 'kiet' } });
    fireEvent.change(screen.getByLabelText('Password'), { target: { value: 'secret1' } });
    fireEvent.change(screen.getByLabelText('Department'), { target: { value: 'Engineering' } });
    fireEvent.click(screen.getByRole('button', { name: 'Sign Up' }));

    expect(onRegister).toHaveBeenCalledWith(expect.objectContaining({
      username: 'Kiet', email: 'kiet@klose.dev', password: 'secret1',
      role: 'staff', permissions: 'view', status: 'active', dept: 'Engineering',
    }));
  });
});

describe('protected routing', () => {
  const renderProtectedRoute = () => render(
    <MemoryRouter initialEntries={['/dashboard']}>
      <Routes>
        <Route path="/login" element={<div>Login page</div>} />
        <Route element={<ProtectedRoute />}>
          <Route path="/dashboard" element={<div>Dashboard page</div>} />
        </Route>
      </Routes>
    </MemoryRouter>,
  );

  it('redirects an anonymous visitor to login', () => {
    renderProtectedRoute();
    expect(screen.getByText('Login page')).toBeInTheDocument();
  });

  it('renders child routes for an authenticated visitor', () => {
    authStorage.setSession({ token: 'jwt-token', user: {} });
    renderProtectedRoute();
    expect(screen.getByText('Dashboard page')).toBeInTheDocument();
  });
});
