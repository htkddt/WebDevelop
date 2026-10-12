import React from 'react';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import ChatWidget from '../../frontend/src/components/ChatWidget';
import { Leftbar } from '../../frontend/src/components/Leftbar';
import Topbar from '../../frontend/src/components/Topbar';
import { authStorage } from '../../frontend/src/services/authStorage';
import { chatApi } from '../../frontend/src/services/api';

jest.mock('../../frontend/src/services/api', () => ({
  chatApi: { sendMessage: jest.fn() },
}));

describe('navigation controls', () => {
  it('expands the sidebar and exposes its labels', () => {
    render(<MemoryRouter><Leftbar /></MemoryRouter>);
    fireEvent.click(screen.getByRole('button', { name: 'Expand navigation' }));

    expect(screen.getByRole('button', { name: 'Collapse navigation' })).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByText('Employees')).toBeInTheDocument();
  });

  it('clears authentication and returns to login from the profile menu', () => {
    authStorage.setSession({ token: 'jwt-token', user: {} });
    render(<MemoryRouter initialEntries={['/dashboard']}><Topbar /></MemoryRouter>);

    fireEvent.click(screen.getByRole('button', { name: 'Open user menu' }));
    fireEvent.click(screen.getByRole('button', { name: /Logout/ }));

    expect(authStorage.getToken()).toBeNull();
  });
});

describe('chat widget', () => {
  it('sends non-empty messages and displays the AI reply', async () => {
    chatApi.sendMessage.mockResolvedValue({ header: 'Klose: ', reply: 'Hello!' });
    render(<MemoryRouter><ChatWidget /></MemoryRouter>);

    fireEvent.click(screen.getByRole('button', { name: 'Open chat' }));
    fireEvent.change(screen.getByPlaceholderText('Ask Klose'), { target: { value: 'How are you?' } });
    fireEvent.click(screen.getByRole('button', { name: '' }));

    expect(chatApi.sendMessage).toHaveBeenCalledWith('How are you?');
    await waitFor(() => expect(screen.getByText('Klose: Hello!')).toBeInTheDocument());
  });
});
